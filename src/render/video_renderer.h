#pragma once

// Two pass video pipeline.
//
//   pass 1  card format -> RGBA at cropped source resolution
//           (colour matrix, range, bob deinterlace, crop)
//   pass 2  RGBA -> window rectangle
//           (nearest / bilinear / bicubic / lanczos / sharp bilinear, sharpen)
//
// Splitting it this way keeps every scaling filter independent of what the card
// happens to deliver, and means the filter always works on real pixels rather
// than on half-decoded chroma.

#include <memory>
#include <vector>
#include <string>

#include "capture/frame_buffer.h"
#include "capture/video_format.h"
#include "common.h"
#include "config.h"
#include "render/render_passes.h"
#include "window.h"

namespace cap {

class Display;

class VideoRenderer {
 public:
  VideoRenderer();
  ~VideoRenderer();

  VideoRenderer(const VideoRenderer&) = delete;
  VideoRenderer& operator=(const VideoRenderer&) = delete;

  bool Initialize(Display* display, std::string* error);
  void Shutdown();

  // Reconfigures the input textures. Cheap when the format did not change.
  bool SetSourceFormat(const VideoFormatInfo& info, std::string* error);

  // Copies one captured frame into GPU memory. Returns false on a size or
  // format mismatch.
  bool UploadFrame(const FrameView& frame);

  // Renders the last uploaded frame into the currently bound back buffer.
  // `fieldIndex` picks the field when bob deinterlacing is on.
  void Draw(const ImageSettings& image, int fieldIndex);

  // Was von den Messungen nicht vor dem Bild fertig sein muss. Einmal je
  // Durchgang, nach dem Present: UploadFrame legt nur ab, was dafuer vom Bild
  // gebraucht wird, und die Rechnung selbst kommt so nicht mehr zwischen
  // Ankunft und Schirm. Ohne neues Bild kostet der Aufruf eine Abfrage.
  void AnalyzeAfterPresent();

  bool hasFrame() const { return hasFrame_; }
  void DropFrame() { hasFrame_ = false; }

  // Where the picture ended up inside the window, in client pixels.
  const Rect& videoRect() const { return videoRect_; }

  // Pixels reserved at the top of the window for the toolbar. The picture is
  // fitted below them rather than drawn underneath, so the bar never covers
  // what you are playing.
  void SetTopInset(int pixels) { topInset_ = pixels < 0 ? 0 : pixels; }

  // Width over height the picture should be shown at, as of the last draw; 0
  // before anything was drawn. Stretch does not change it.
  double pictureAspect() const { return pictureAspect_; }

  // Picture size with `factor` window pixels per output line and the other
  // side following the aspect. False while there is no picture.
  bool PictureSizeAt(int factor, int* w, int* h) const;

  // Samples per cycle of the colour subcarrier, for a full width line. Set from
  // the video standard; the dot crawl filter is built around it.
  //
  // Ein anderer Traeger ist eine andere Norm, und der gemessene Kriechzyklus
  // gehoert zur alten. Dieselbe Zahl noch einmal gesetzt wirft nichts weg.
  void SetCarrierSamples(double samples) {
    if (samples == carrierSamples_) return;
    carrierSamples_ = samples;
    ResetCrawl();
  }

  // Whether the picture arriving is one picture.
  //
  // Sounds like a strange thing to ask, and it is the whole justification for
  // the tile half of the interlacing test. That half assumes a badly combed
  // corner means a combed signal -- true of a console, whose screen is one
  // scene, and false of a captured desktop, which is a picture made of other
  // people's pictures. Any window on it can hold a still or a video that is
  // itself combed, and no test that looks at pixels can tell combing from a
  // photograph of combing.
  //
  // An analogue input is the case where the assumption holds and where the tile
  // test is needed at all, because a card that decodes composite says nothing
  // about fields. Digital sources announce 1080i themselves, so they lose
  // nothing by keeping to the frame-wide test alone -- which is what shipped in
  // 3.0 and is unchanged here.
  void SetAnalogueSource(bool analogue) { analogueSource_ = analogue; }

  // Wie schnell die Bilder ankommen, gemessen; 0, solange nichts gemessen ist.
  //
  // Damit kann die Bildrate ein gemessenes "interlaced" ueberstimmen, und das
  // nicht als Faustregel, sondern weil der Formatraum es hergibt: ein gewebtes
  // Halbbildpaar *ist* ein Bild, und jedes Format, das es gibt, liefert davon
  // 25 oder 30 in der Sekunde -- 576i und 1080i50 fuenfundzwanzig, 480i und
  // 1080i60 knapp dreissig. 720i hat es in keiner Norm je gegeben. Kommen also
  // von einer Quelle mit 720 Zeilen oder mehr fuenfzig oder sechzig Bilder in
  // der Sekunde an, koennen diese Bilder keine gewebten Halbbilder sein, ganz
  // gleich wie sehr sie kaemmen.
  //
  // Gebraucht wird das, weil die Kammpruefung raeumlich misst und ein
  // Grafikstil raeumlich luegen kann. Ein absichtlich gelegtes Zeilenraster,
  // Dithering, eine Kreuzschraffur: alles Muster, die je Abtastung dieselbe
  // Zacke machen wie ein bewegtes Halbbildpaar, und ein Bild, das sie ueber die
  // ganze Flaeche traegt, kaemmt in jeder Kachel. Unter 720 Zeilen bleibt es
  // beim Urteil der Messung -- dort *ist* 480i/576i der Normalfall, und ein
  // Veto naehme der Erkennung genau die Faelle weg, fuer die es sie gibt.
  //
  // Die *gemessene* Rate, nicht die angekuendigte. Diese Karte kuendigt bei
  // einer Halbbildquelle 50 an und liefert 25 gewebte Bilder; die
  // angekuendigte Zahl wuerde also genau die Quellen wegwerfen, um die es
  // hier geht.
  void SetFrameRateHint(double fps) { frameRateHint_ = fps > 0.0 ? fps : 0.0; }

  // Samples per cycle of the subcarrier at the width actually being captured --
  // what the shader works with, and what the settings need to say honestly how
  // wide the demodulation window currently is.
  float effectiveCarrierPeriod() const {
    const int w = source_.width > 0 ? source_.width : 720;
    return (float)(carrierSamples_ * (double)w / 720.0);
  }

  const VideoFormatInfo& sourceFormat() const { return source_; }
  // What curve the incoming picture is encoded against. Kept apart from the
  // format: a card can send eight bit PQ, and ten bit says nothing about HDR.
  enum class Transfer { Sdr = 0, Pq = 1, Hlg = 2 };

  // Told rather than guessed, because a DirectShow media type mostly does not
  // carry this and when it does it is worth believing. `wideGamut` says the
  // primaries are BT.2020 and want bringing back to BT.709.
  void SetHdrInput(Transfer transfer, bool wideGamut) {
    hdrTransfer_ = transfer;
    hdrWideGamut_ = wideGamut;
  }
  Transfer hdrTransfer() const { return hdrTransfer_; }

  // The interface, when the swapchain is scRGB. Between these two calls the
  // interface draws into a buffer of its own; the second brings it back over
  // the picture in linear light. Both do nothing at all in SDR.
  bool BeginUiLayer();
  void CompositeUiLayer();

  // Where the picture is going. Nits, because that is the only unit in which
  // the two ends of this can be compared at all.
  void SetHdrOutput(bool scRgbOutput, float paperWhiteNits, float sourcePeakNits,
                    float displayPeakNits) {
    hdrOutput_ = scRgbOutput;
    paperWhiteNits_ = paperWhiteNits;
    sourcePeakNits_ = sourcePeakNits;
    displayPeakNits_ = displayPeakNits;
  }

  // True when the current source is interlaced according to its media type.
  bool sourceInterlaced() const { return source_.interlaced; }

  // Effective size after cropping.
  // Size of the picture this hands out: cropped, line doubled, rotated, and --
  // unless that was switched off -- resampled to square pixels. This is what
  // the recorder, the screenshots and the virtual camera get, so it is the size
  // that counts everywhere outside the shader.
  //
  // Not the same as the intermediate's size any more. The window has always
  // been able to show non-square pixels for free, by drawing into a rectangle
  // of the right shape; a file has nowhere to put that, so on the way out the
  // picture is resampled instead. On HDMI the two sizes are equal and nothing
  // happens.
  int outputWidth() const { return deliveryWidth_ > 0 ? deliveryWidth_ : outputWidth_; }
  int outputHeight() const { return deliveryHeight_ > 0 ? deliveryHeight_ : outputHeight_; }

  // Ausgabezeilen je echter Bildzeile, wie sie beim letzten Zeichnen anlagen.
  // Zwei ist die Grenze, unter der keine Zeilenluecke mehr hineinpasst; 0 heisst
  // gedreht oder noch nichts gezeichnet. Steht hier, weil die Zahl aus dem
  // Fenster, dem Zuschnitt und der angesagten Zeilenzahl zugleich kommt und
  // sonst niemand alle drei beisammen hat.
  float scanlineRoom() const { return scanlineRoom_; }

  // ---- readback for recording ----
  //
  // Hands out the intermediate image -- cropped, deinterlaced, colour corrected,
  // at source resolution, before any display scaling. That is exactly what the
  // viewer shows and exactly what a recording should contain, and it does not
  // depend on the window size.
  //
  // The copy is queued on the GPU and read two frames later, so the display path
  // never waits on it. The recording is therefore a frame or two behind the
  // screen, which is the right trade: the screen is what you play on.
  void SetReadbackEnabled(bool enabled);
  bool readbackEnabled() const { return readbackEnabled_; }

  // Byte order the readback delivers, written the way ffmpeg names it. The
  // bytes run R, G, B, A -- that is ffmpeg's "rgba", not "bgra". Whoever
  // changes the target the backend copies from changes this line in the same
  // edit. Getting it wrong is not a crash: red and blue simply trade places,
  // and orange comes back blue.
  static constexpr const char* kReadbackPixelFormat = "rgba";

  // The same picture for a recording that keeps the range: ten bits per
  // component, PQ encoded, BT.2020, packed with red in the low bits. That is
  // what ffmpeg calls x2bgr10le -- the name looks wrong until you remember it
  // describes the bytes, not the order they are written in.
  static constexpr const char* kHdrReadbackPixelFormat = "x2bgr10le";


  struct ReadbackFrame {
    const uint8_t* data = nullptr;  // kReadbackPixelFormat, valid until ReleaseReadback
    size_t size = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool valid() const { return data != nullptr; }
  };

  // Returns true when a frame was mapped; call ReleaseReadback when done with it.
  bool FetchReadback(ReadbackFrame* out);
  void ReleaseReadback();
  // Ask for the wide path. Two askers -- the recorder and the virtual camera --
  // and one ring between them, because they want the same bytes. Ignored unless
  // the source is actually HDR; there is nothing to keep otherwise.
  void SetHdrWideWanted(bool recorder, bool camera) {
    hdrRecordWanted_ = recorder;
    hdrCameraWanted_ = camera;
  }
  bool hdrWideActive() const {
    return (hdrRecordWanted_ || hdrCameraWanted_) && hdrTransfer_ != Transfer::Sdr;
  }
  bool FetchHdrReadback(ReadbackFrame* out);

  // The picture as linear light, four half floats per pixel, 1.0 being diffuse
  // white. For a screenshot that keeps the range. Blocks on the GPU, which a
  // still is allowed to do -- it happens when somebody presses a key, not sixty
  // times a second.
  bool GrabStillHalf(std::vector<uint16_t>* out, int* width, int* height, int* strideBytes);
  void ReleaseHdrReadback();

  // ---- automatic level detection ----
  //
  // What a capture card hands over is limited range (16-235) or full range
  // (0-255) depending on what the *source* is sending -- a console setting, not
  // a property of the pixel format you asked the card for. Plenty of cards,
  // this one included, attach no colour description at all, so there is nothing
  // to read: the answer has to come from the pixels.
  //
  // Measured over the first couple of seconds after the format changes, then
  // frozen. A verdict that keeps changing with the content would be worse than
  // a wrong one held steady, so it is decided once and left alone.
  enum class RangeVerdict { Pending, Limited, Full };
  RangeVerdict detectedRange() const { return rangeVerdict_; }

  // Die Zahlen, aus denen das Urteil gefallen ist. "Begrenzt" oder "voll" ist
  // die Antwort auf die Frage, die der Renderer hat; wer wissen will, was eine
  // Treibereinstellung angerichtet hat, braucht die Frage davor: liegt Schwarz
  // bei 0 oder bei 16. Dieselbe Auskunft steht im Log, aber genau einmal, in
  // dem Augenblick, in dem sie feststand.
  //
  // Ohne Sperre gelesen, wie das Urteil selbst: geschrieben wird beim Messen,
  // gelesen beim Zeichnen. Der schlimmste Fall ist ein Satz Zahlen aus zwei
  // aufeinanderfolgenden Bildern -- fuer eine Anzeige belanglos, und eine
  // Sperre in den Messpfad zu legen waere es nicht wert.
  struct RangeNumbers {
    int min = 255;
    int max = 0;
    unsigned long long below16 = 0;
    unsigned long long above235 = 0;
    unsigned long long samples = 0;
  };
  RangeNumbers rangeNumbers() const {
    RangeNumbers n;
    n.min = rangeMin_;
    n.max = rangeMax_;
    n.below16 = rangeBelow16_;
    n.above235 = rangeAbove235_;
    n.samples = rangeSamples_;
    return n;
  }

  // Whether the source is interlaced, measured the same way and for the same
  // reason: the media type is the obvious place to ask and routinely does not
  // answer. A plain VIDEOINFOHEADER has no field to say it in, and cards that do
  // use VIDEOINFOHEADER2 often leave the interlace flags at zero -- so a 480i
  // console can arrive described as progressive.
  //
  // Unlike the range verdict this one is never frozen on "progressive". Combing
  // only exists where something moved, and a paused game or a title screen has
  // nothing moving in it; calling that progressive and sticking to it would
  // leave the deinterlacer switched off for the rest of the session. So the
  // measurement runs in windows and keeps running, and only "interlaced" latches
  // -- once combing has been seen there is no reason to doubt it again.
  enum class InterlaceVerdict { Pending, Progressive, Interlaced };
  InterlaceVerdict detectedInterlace() const { return interlaceVerdict_; }

  // Wie farbig das Bild seit dem letzten ResetChroma war, 0..1. Gemessen wird
  // die Farbe *ueber einen Block gemittelt*, nicht die einzelner Bildpunkte --
  // der Grund dafuer steht bei AnalyzeChroma und ist der Kern der Sache. -1,
  // solange zu wenige Bilder gemessen wurden oder das Format keine lesbare
  // Farbe hat.
  //
  // Wofuer das da ist: der Lock der Karte misst nur die Zeilenfrequenz. Steht
  // der falsche Farbtraeger, rastet sie trotzdem ein, der Farbkiller des
  // Decoders greift mangels Burst -- und heraus kommt ein graues Bild mit
  // Regenbogengries. Genau das ist hier messbar und sonst nirgends.
  float chromaEnergy() const;
  // Dieselbe Messung, aber nur ueber die *dunklen* Bloecke. -1, wenn es zu
  // wenige davon gab, um etwas zu sagen.
  //
  // Wofuer das da ist: `chromaEnergy` beantwortet "ist ueberhaupt Farbe da",
  // und das reicht nicht. Ein falscher Farbtraeger kann die Farbe auch
  // *erfinden* statt sie zu toeten -- am 30.08. rastete die Suche auf einer
  // PAL-Quelle versehentlich auf SECAM B ein, und heraus kam ein Bild mit
  // rotem Schwarz und 0,241 gemessener Farbe, dem Achtfachen dessen, was das
  // richtige PAL B auf derselben Szene misst (0,029). Nach Menge waere also
  // die falsche Norm der Sieger.
  //
  // Was die beiden Faelle trennt, ist das dunkle Ende: bei richtiger Norm ist
  // es neutral, immer, egal wie bunt der Rest ist. Farbe dort ist nicht Farbe,
  // sondern ein Traeger, den es nicht gibt.
  //
  // Dunkel heisst dabei *nach Luma*, nicht nach Helligkeit. Ein falscher
  // Traeger verschiebt U und V, nicht Y -- rechnet man die Helligkeit aus dem
  // fertigen RGB, faellt rotes Schwarz aus der Auswahl heraus und die Messung
  // findet genau die Bloecke nicht, wegen derer sie da ist.
  float darkChromaEnergy() const;
  // Wie viel vom gemessenen Bild ueberhaupt Licht hatte, 0..1: der Anteil der
  // Bloecke, die *nicht* als dunkel gezaehlt wurden. -1, solange zu wenige
  // Bilder gemessen wurden.
  //
  // Wofuer das da ist: die beiden Werte oben sagen, wie farbig das Bild war,
  // nicht ob ueberhaupt eines da war. Ein fast schwarzes Bild misst in jeder
  // Norm dieselbe Null -- Schwarz ist in allen Normen schwarz --, und eine
  // Messung, die in jeder Norm gleich ausfaellt, kann zwischen Normen nicht
  // unterscheiden. Ein Vergleich braucht also nicht nur Farbe, sondern zuerst
  // beleuchtete Flaeche, in der Farbe stehen koennte.
  //
  // Dieselben Bloecke, dasselbe Fenster und derselbe Beschnitt wie chromaEnergy
  // und darkChromaEnergy, absichtlich: was hier gezaehlt wird, ist genau das,
  // woraus die beiden anderen Zahlen entstanden sind.
  float chromaLitFraction() const;
  // Wie stark die Farbe von Halbbildzeile zu Halbbildzeile *umklappt*, auf der
  // V-Achse (R-Y) und auf der U-Achse (B-Y) getrennt. -1, solange zu wenige
  // Bilder gemessen wurden oder das Format keine Zeilen mit voller
  // Farbaufloesung hat.
  //
  // Wofuer das da ist, steht bei AnalyzeChroma: PAL kehrt die V-Phase in jeder
  // Zeile um, NTSC nicht. Ein NTSC-Dekoder auf einem PAL-Signal dreht sie
  // nicht zurueck, und dann steht der Unterschied zwischen den beiden Normen
  // nicht in der Farbmenge, sondern hier.
  float chromaAltV() const;
  float chromaAltU() const;
  // Von vorn messen. Wird gerufen, wenn die Norm gewechselt hat: was vor dem
  // Wechsel gemessen wurde, gehoert zu einer anderen Einstellung.
  void ResetChroma();

  // Wie dicht gemessen wird: jedes `everyNth`-te Bild.
  //
  // Im Normalbetrieb duenn, denn die Messung laeuft dauerhaft mit und soll
  // billig sein; gebraucht wird dort kein genauer Wert, sondern die
  // Unterscheidung "farbig" von "grau". Waehrend eines Normenvergleichs
  // dagegen dicht, und zwar nicht
  // aus Ungeduld: die Kandidaten sollen dieselbe Szene sehen. Bei jedem achten
  // Bild dauert eine Messung 3,2 s und der ganze Rundgang vierzehn, und in
  // vierzehn Sekunden ist eine Spielkonsole im Vorschaumodus zwei Szenen
  // weiter -- gemessen wurde dann zum Teil das Programm und nicht die Norm.
  // Dicht abgetastet kostet dieselbe Zahl Messbilder 0,4 s; ein Rundgang aus
  // vier Messungen brauchte am 30.08. gemessen 2,9 s statt vierzehn.
  //
  // Die Zahl der Messbilder aendert sich dabei *nicht*: die dunklen Bloecke
  // muessen sich weiter ueber zehn Bilder ansammeln, sonst hat die Messung, auf
  // die es ankommt, keine Grundlage.
  void SetChromaCadence(int everyNth);

  // True when the two fields hold the *same* lines rather than lines half a
  // picture line apart -- a 240p or 288p console that the card packed into an
  // interlaced frame. Such a source is still interlaced in the sense that
  // matters, because the two fields are half a field apart in time and will comb
  // on anything that moves. What it does not need is spatial reconstruction:
  // nothing is missing, so the right answer is to take the field's own line and
  // use it twice.
  bool sourceCoSitedFields() const { return coSitedFields_; }

  // Nach wie vielen Bildern das Punktkriechen wieder in derselben Phase steht,
  // gemessen an dieser Quelle: 2, 3 oder 4. 0, solange nichts gemessen ist.
  //
  // Wofuer das da ist: der zeitliche Filter nimmt das Kriechen heraus, indem er
  // ueber genau einen Umlauf der Traegerphase mittelt, und wie lang ein Umlauf
  // ist, legt die Quelle fest und nicht die Norm. Das PAL-Signal, an dem die
  // vier gemessen wurden, zeigt Bilddifferenzen von 1,91, 2,62, 1,90 und 0,78
  // bei einem bis vier Bildern Abstand -- ein sauberer Zyklus von vier. Ein
  // NTSC-Signal nach Lehrbuch kippt die Phase in jedem Bild und ist nach zweien
  // zurueck, und eine Konsole, die ihren Takt nicht nach der Norm waehlt, kann
  // bei drei landen. Ueber vier gemittelt bleibt von einem Zyklus aus drei ein
  // Viertel des Musters stehen, und ueber zwei gemittelt von einem Zyklus aus
  // vier sieben Zehntel.
  //
  // Solange nichts gemessen ist, mittelt der Filter ueber vier Bilder, wie vor
  // dieser Messung fest eingebaut. Gemessen wird nur, solange er an ist und die
  // Quelle analog -- ein Filter, den niemand eingeschaltet hat, kostet nichts.
  int crawlCycle() const { return crawlCycle_; }

  // Where the picture sits inside the frame, in source pixels, edges inclusive.
  // False until something has been measured. This is what the automatic crop
  // reads: a console pillarboxed inside a 720 pixel line, or an overscan band
  // the card hands over as black, is found here rather than by eye.
  bool contentBounds(int* left, int* top, int* right, int* bottom) const;

  // ---- is there a picture in these frames at all ----
  //
  // Asking whether frames are arriving answers this on a digital input and not
  // at all on an analogue one: a card with nothing on its composite pin goes on
  // delivering sixty frames a second of whatever the decoder makes of an open
  // wire. So the question has to be put to the pixels.
  //
  // No signal has two appearances and they look nothing like each other. An
  // unterminated analogue input is *snow* -- full of contrast, and completely
  // different from one frame to the next. A card that mutes, or a decoder that
  // never locks, gives a flat field instead, with no contrast at all.
  //
  // Snow is decidable: that much contrast with that little correlation cannot
  // be a picture. Flat is not -- a black loading screen is the same measurement
  // -- so that one is only ever reported after it has held for a while, and the
  // caller decides how long. Nothing here latches: a signal can come and go.
  enum class SignalVerdict { Unknown, Picture, Snow, Flat };
  SignalVerdict detectedSignal() const { return signalVerdict_; }
  // Seconds the current verdict has held, or 0 before anything is measured.
  double signalHeldSeconds() const;

  // Throws every measurement away and starts over. Switching the crossbar puts a
  // different signal on the same pins without the format changing, and none of
  // the verdicts survive that.
  void ResetAnalysis();

  // Nur den Wertebereich. Das ist die eine Messung, die man von Hand wiederholen
  // will: sie haengt an Einstellungen ausserhalb von qBlank -- Treiberoption,
  // Quellgeraet -- die sich aendern koennen, ohne dass am Format etwas
  // passiert. Die Verschraenkung und die Bildgrenzen davon mitzureissen waere
  // nur schaedlich, denn beide zu verlieren sieht man sofort.
  void ResetRangeAnalysis();

  // Copies the current picture out once, tightly packed, in
  // kReadbackPixelFormat. Unlike the recording path this waits for the GPU,
  // which costs a millisecond or two -- acceptable for a key press, not for
  // every frame. Alpha comes back opaque. Must be called on the render thread.
  bool GrabStill(std::vector<uint8_t>* pixels, int* width, int* height);

 private:
  void QueueReadback();
  void QueueHdrReadback();
  bool EnsureHdrRecord(int width, int height);
  void ReleaseReadbackResources();

 public:

 private:
  // 7 is NV12's arrangement in sixteen bit containers -- P010 and P016, which
  // is what an HDR capable card hands over.
  enum class FormatKind {
    Yuy2 = 0, Uyvy = 1, Yvyu = 2, Nv12 = 3, Planar420 = 4, Rgb = 6, P010 = 7
  };


  bool CreateSourceTextures(std::string* error);
  bool EnsureIntermediate(int width, int height);
  // The pass between the intermediate and everything that is not the window.
  // "half" picks the linear light target, which is what the PQ recording and
  // the wide screenshot read; the other one is an ordinary eight bit picture.
  bool RenderDelivery(bool half);
  void ComputeDeliverySize(const ImageSettings& image);
  // Shape the picture should be seen in, as width over height. Cropping,
  // rotation and the media type's own aspect all land here; the window and the
  // delivery pass then each get there their own way.
  double TargetAspect(const ImageSettings& image) const;
  bool deliveryResize() const {
    return deliveryWidth_ != outputWidth_ || deliveryHeight_ != outputHeight_;
  }
  // Whether any knob is off its centre. Four comparisons rather than a flag, so
  // that a user who zeroes the sliders again gets the pass switched back off
  // instead of paying for it until the next restart.
  bool procAmpActive() const {
    return deliveryBrightness_ != 0.0f || deliveryContrast_ != 1.0f ||
           deliverySaturation_ != 1.0f || deliveryHue_ != 0.0f;
  }
  // Whether the pass has to run at all. Three separate reasons, and any one of
  // them is enough: the picture has to change shape, it has to come down out of
  // HDR, or the user wants the picture controls in the file as well. The third
  // is why this is not simply deliveryResize().
  bool deliveryNeeded() const {
    return deliveryResize() || hdrTransfer_ != Transfer::Sdr ||
           (deliveryProcAmp_ && procAmpActive());
  }
  // The same question for the linear light target, where the HDR reason drops
  // out: that path is not coming down to SDR, it stays wide, so a source that
  // is already the right shape with the knobs centred has nothing to do.
  bool deliveryHalfNeeded() const {
    return deliveryResize() || (deliveryProcAmp_ && procAmpActive());
  }
  void ReleaseSourceTextures();

  bool UploadPacked(const FrameView& frame);
  bool UploadNv12(const FrameView& frame);
  bool UploadP010(const FrameView& frame);
  bool UploadPlanar(const FrameView& frame);
  bool UploadRgb24(const FrameView& frame);
  bool UploadRgb32(const FrameView& frame);

  void ComputeDestRect(const ImageSettings& image);
  void ComputeDestRectIn(const ImageSettings& image, int winW, int winH);

  // Everything a graphics API owns lives behind this one member: the textures,
  // the shaders, the passes themselves. What is left in this class is the
  // deciding -- which pass to run, at what size, with which numbers.
  std::unique_ptr<RenderPasses> passes_;
  // The window's device and back buffer. Not owned.
  Display* display_ = nullptr;

  // Input planes. How many of these exist depends on the source format.
  int planeCount_ = 0;

  static const int kHistoryDepth = RenderPasses::kHistoryDepth;
  int historyWrite_ = 0;   // ring slot the next copy goes into
  int historyCount_ = 0;   // slots that hold a picture, up to kHistoryDepth
  bool historyWanted_ = false;

  VideoFormatInfo source_;
  bool tenBitContainer_ = false;
  Transfer hdrTransfer_ = Transfer::Sdr;
  bool hdrWideGamut_ = false;
  bool hdrOutput_ = false;
  float paperWhiteNits_ = 203.0f;
  float sourcePeakNits_ = 1000.0f;
  float displayPeakNits_ = 100.0f;
  FormatKind kind_ = FormatKind::Rgb;
  bool planarUvSwapped_ = false;  // YV12 stores V before U
  bool hasFrame_ = false;

  int croppedWidth_ = 0;
  int croppedHeight_ = 0;
  int outputWidth_ = 0;
  int outputHeight_ = 0;
  // Size the picture leaves at, and the filter that gets it there. Worked out
  // once per frame in Draw, because that is the only place holding the settings;
  // the readback and the stills run outside it and read these.
  int deliveryWidth_ = 0;
  int deliveryHeight_ = 0;
  float scanlineRoom_ = 0.0f;
  int deliveryFilter_ = 0;
  // The picture controls, cached from the same place and for the same reason.
  // Kept in radians already, because the shader wants them that way and Draw is
  // the one place that runs per frame anyway.
  float deliveryBrightness_ = 0.0f;
  float deliveryContrast_ = 1.0f;
  float deliverySaturation_ = 1.0f;
  float deliveryHue_ = 0.0f;
  bool deliveryProcAmp_ = false;  // the user wants them past the window too
  // The A/B comparison as the scale pass needs it: which part of the picture
  // gets none of that pass's effects. Negative split = no comparison.
  float compareSplit_ = -1.0f;
  int compareAxis_ = 0;
  int rotation_ = 0;
  // Last sizes written to the log, so the line appears when it changes rather
  // than sixty times a second.
  int loggedDeliveryW_ = -1;
  int loggedDeliveryH_ = -1;
  int loggedOutW_ = -1;
  int loggedOutH_ = -1;
  Rect videoRect_ = {};
  int topInset_ = 0;
  double pictureAspect_ = 0.0;
  bool pictureTurned_ = false;
  double carrierSamples_ = 3.0449;  // PAL, the common case here

  static const int kReadbackSlots = RenderPasses::kReadbackSlots;

  // The recording path when the range is being kept. A ring of its own rather
  // than a mode on the one above: the camera and the screenshots still want an
  // ordinary eight bit picture at the same moment.
  bool hdrRecordWanted_ = false;
  bool hdrCameraWanted_ = false;
  int hdrReadWrite_ = 0;
  int hdrReadQueued_ = 0;
  int hdrReadMapped_ = -1;
  int readbackWidth_ = 0;
  int readbackHeight_ = 0;
  int readbackWrite_ = 0;
  int readbackQueued_ = 0;
  int readbackMapped_ = -1;  // slot currently mapped, -1 when none
  bool readbackEnabled_ = false;

  // Level detection state, reset whenever the source format changes.
  void AnalyzeLevels(const FrameView& frame);
  void AnalyzeInterlace(const FrameView& frame);
  void AnalyzeContentBounds(const FrameView& frame);
  void AnalyzeSignal(const FrameView& frame);
  void AnalyzeChroma(const FrameView& frame);
  void SampleCrawl(const FrameView& frame);
  void AnalyzeCrawl();
  void JudgeCrawl();
  void ResetCrawl();
  // Byte offset of the first luma sample and the distance to the next one.
  // False for formats this cannot read.
  bool LumaLayout(size_t* offset, size_t* step) const;
  // Dasselbe fuer die beiden Farbdifferenzen -- nur reicht dafuer kein Paar aus
  // Versatz und Schrittweite, weil die Farbe je nach Format entweder neben dem
  // Luma oder in einer eigenen Ebene liegt und mal nur waagerecht, mal auch
  // senkrecht unterabgetastet ist.
  //
  // Beides faellt auf dieselbe Beschreibung zusammen: Luma des Bildpunktes
  // (x, y) steht bei `yOff + y*yPitch + x*yStep`, seine Farbe bei
  // `uOff + (y >> cyShift)*uPitch + (x >> cxShift)*uStep`. Damit muss die
  // Messschleife den Unterschied nicht mehr kennen, und ein weiteres Format
  // kostet einen Zweig hier statt einer zweiten Schleife dort.
  struct ChromaPlanes {
    size_t yOff = 0, uOff = 0, vOff = 0;
    size_t yPitch = 0, uPitch = 0, vPitch = 0;
    size_t yStep = 1, uStep = 1, vStep = 1;
    int cxShift = 0, cyShift = 0;
    size_t needed = 0;  // so gross muss das Bild mindestens sein
  };
  bool ChromaLayout(ChromaPlanes* planes) const;
  RangeVerdict rangeVerdict_ = RangeVerdict::Pending;
  int rangeFramesSeen_ = 0;
  uint64_t rangeSamples_ = 0;
  uint64_t rangeBelow16_ = 0;
  uint64_t rangeAbove235_ = 0;
  int rangeMin_ = 255;
  int rangeMax_ = 0;

  // Farbstaerke. Laeuft dauerhaft mit und latcht nicht: anders als Pegel und
  // Halbbilder ist das keine Eigenschaft der Quelle, sondern eine der gerade
  // eingestellten Norm, und die kann sich jederzeit aendern.
  int chromaFramesSeen_ = 0;
  int chromaFramesAnalysed_ = 0;
  int chromaSampleEvery_ = 8;  // siehe SetChromaCadence
  int chromaFramesWanted_ = 10;  // haengt am Takt, siehe SetChromaCadence
  uint64_t chromaSum_ = 0;
  uint64_t chromaCount_ = 0;
  // Dieselbe Summe, aber nur ueber die dunklen Bloecke. Eigene Zaehlung, weil
  // ein Bild auch ganz ohne dunkle Stellen auskommen kann.
  uint64_t chromaDarkSum_ = 0;
  uint64_t chromaDarkCount_ = 0;
  // Die Zeilenalternation, eigene Zaehlung: sie braucht vier Halbbildzeilen je
  // Block statt zwei und faellt deshalb an anderen Bloecken aus als die
  // Summen darueber.
  uint64_t chromaAltVSum_ = 0;
  uint64_t chromaAltUSum_ = 0;
  uint64_t chromaAltCount_ = 0;

  InterlaceVerdict interlaceVerdict_ = InterlaceVerdict::Pending;
  bool coSitedFields_ = false;
  // 0 when a pair starts on an even row, 1 when it starts on an odd one.
  int coSitedPhase_ = 0;
  int combFramesSeen_ = 0;
  uint64_t combSamples_ = 0;
  uint64_t combHits_ = 0;
  // How many of the frames in the current window combed. Counting frames rather
  // than averaging over all of them is the difference between noticing a second
  // of movement in an otherwise still scene and averaging it away.
  int combFrameHits_ = 0;
  int combFramesAnalysed_ = 0;
  // Of those, the ones only a single tile noticed -- a small thing moving in an
  // otherwise still picture. Diagnostic only.
  int combTileOnly_ = 0;
  // Whether the tile half of the test is allowed to decide. See
  // SetAnalogueSource.
  bool analogueSource_ = false;
  // Gemessene Ankunftsrate, 0 = noch nicht gemessen. Siehe SetFrameRateHint.
  double frameRateHint_ = 0.0;
  // Ob schon im Log steht, dass die Rate ein Kammurteil verworfen hat. Einmal
  // je Format, nicht je Fenster: sonst schriebe eine gerasterte Grafik jede
  // Sekunde dieselbe Zeile.
  bool rateVetoLogged_ = false;
  // Worst reading of the current window, whole frame and best tile. Only ever
  // read back in the log line.
  double combFrameBest_ = 0.0;
  double combTileBest_ = 0.0;
  // Sums of the difference between the two rows of a pair, and between two
  // neighbouring pairs. On a line doubled picture the first is nearly zero.
  uint64_t pairInner_ = 0;
  uint64_t pairOuter_ = 0;

  // Kriechzyklus, siehe crawlCycle() und AnalyzeCrawl.
  bool crawlWanted_ = false;  // der zeitliche Filter ist an
  int crawlCycle_ = 0;        // uebernommen; 0 = noch nichts gemessen
  int crawlCandidate_ = 0;    // was das letzte eindeutige Fenster sagte
  int crawlLogged_ = -1;      // letztes Fensterurteil im Log, -1 = keins
  // Das Messraster, gebaut fuer eine Groesse und einen Traeger. Je Stelle
  // Spalte und Zeile; je Abgriff das Fenstergewicht und dasselbe mal Kosinus
  // und Sinus des Traegers.
  int crawlWidth_ = 0;
  int crawlHeight_ = 0;
  float crawlPeriod_ = 0.0f;
  size_t crawlStep_ = 0;  // Bytes von einem Lumawert zum naechsten
  int crawlReach_ = 0;    // Abgriffe links und rechts der Stelle
  std::vector<int> crawlSiteX_;
  std::vector<int> crawlSiteY_;
  std::vector<float> crawlTapW_, crawlTapC_, crawlTapS_;
  float crawlSumW_ = 0.0f, crawlSumC_ = 0.0f, crawlSumS_ = 0.0f;
  // Die letzten dreizehn Bilder: je Stelle der Zeiger im Traegerband (zwei
  // Zahlen) und der Mittelwert des Fensters, abgelegt auf dem Platz Bildnummer
  // modulo dreizehn, dazu je Platz die Bildnummer (0 = leer). Gebraucht werden
  // die Abstaende eins bis vier, die der Shader mitteln kann, und zwoelf fuer
  // den Ruhetest.
  std::vector<float> crawlRing_;
  std::vector<uint64_t> crawlRingSequence_;
  uint64_t crawlLastSequence_ = 0;
  // Was UploadFrame vom Bild abgelegt hat und AnalyzeAfterPresent noch
  // auswerten muss: je Stelle die Bytes unter dem Fenster, wie sie im Bild
  // stehen, dazu die Bildnummer (0 = nichts abgelegt).
  std::vector<uint8_t> crawlPending_;
  uint64_t crawlPendingSequence_ = 0;
  // Je Stelle der Abstand ueber zwoelf Bilder im Quadrat, -1 = bewegt, und
  // dieselben Zahlen ohne die bewegten, fuer den Median. Mitglieder nur, damit
  // nicht jedes Bild neu anlegt.
  std::vector<float> crawlScratch_;
  std::vector<float> crawlRecur_;
  // Das laufende Messfenster: Abstaende eins bis vier und zwoelf.
  int crawlFrames_ = 0;
  uint64_t crawlStill_ = 0;
  double crawlDiff_[5] = {};

  // Signal presence. The previous sample set is kept rather than the previous
  // frame: the comparison only ever looks at the same sparse grid, so a few
  // hundred bytes stand in for a megabyte.
  SignalVerdict signalVerdict_ = SignalVerdict::Unknown;
  int signalFramesSeen_ = 0;
  std::vector<uint8_t> signalPrev_;
  // When the current verdict was first reached, on the coarse tick counter.
  // Zero before anything is measured. Milliseconds are ample for "how long has
  // this held"; the steady clock would be precision nobody reads.
  uint32_t signalSinceTick_ = 0;

  // Content bounds. Measured as a union across a window of frames, because a
  // fade to black is not evidence that the picture got smaller, and published
  // only once the window is complete so the answer never flickers.
  bool boundsValid_ = false;
  int boundsL_ = 0, boundsT_ = 0, boundsR_ = 0, boundsB_ = 0;
  int accL_ = 0, accT_ = 0, accR_ = 0, accB_ = 0;
  bool accAny_ = false;
  int boundsFramesSeen_ = 0;
  std::vector<int> columnHits_;    // scratch, sized once per format
  std::vector<uint8_t> rowLit_;    // one entry per scanned line, likewise
  std::vector<uint8_t> colLit_;
};

}  // namespace cap
