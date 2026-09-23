#include "ui/settings_search.h"

#include <algorithm>
#include <cstdlib>

#include "i18n.h"

namespace cap {
namespace {

// Die Suche kennt jede Einstellung dreimal: unter dem Namen, der auf dem
// Bildschirm steht, unter dem aus der anderen Sprache, und unter den Woertern,
// die jemand benutzt, der den Namen nicht kennt. Der letzte Teil ist der
// eigentliche Wert -- wer "Gamma" sucht, weiss nicht, dass es hier
// "Helligkeit" heisst, und wer es weiss, braucht die Suche kaum.
//
// Die Tabelle steht in der Reihenfolge der Reiter. Wo ein Regler liegt, wird
// daraus abgeleitet: der letzte Reiter und der letzte Abschnitt davor.
//
// Place names a heading only for the "Tab › Section" beside a result. It is
// never a result itself where a control of the same name sits right under it.
enum class Kind { Tab, Section, Place, Control };

struct Entry {
  Kind kind;
  int tab;
  const char* key;  // the anchor in settings_window.cpp; nullptr for a tab
  const char* de;
  const char* en;
  const char* syn;  // '|' between phrases, either language
};

const Entry kEntries[] = {
    // ---- source
    {Kind::Tab, kTabSource, nullptr, "Quelle", "Source",
     "Gerät|Device|Capture Card|Karte|Grabber"},
    {Kind::Section, kTabSource, "videodev", "Videogerät", "Video device",
     "Capture Card|Aufnahmekarte|Grabber|Karte|Card|USB"},
    {Kind::Control, kTabSource, "input", "Eingang", "Input",
     "HDMI|Composite|FBAS|Cinch|S-Video|Component|YPbPr|RGB|Scart|DVI|SDI|Signal|Anschluss"},
    {Kind::Control, kTabSource, "refresh", "Aktualisieren", "Refresh",
     "Neu laden|Reload|Rescan|Geräteliste|Device list"},
    {Kind::Control, kTabSource, "cardcfg", "Karte konfigurieren", "Configure card",
     "Treiber|Driver|Eigenschaften|Properties|Treiberdialog"},
    {Kind::Control, kTabSource, "cardreinit", "Karte neu einlesen", "Reinitialise card",
     "Reset|Neustart|Restart|hängt|Frozen|schwarzes Bild|Black screen|kein Bild|No picture"},
    {Kind::Section, kTabSource, "vstandard", "Videonorm", "Video standard",
     "PAL|NTSC|SECAM|PAL-M|PAL-N|PAL60|50 Hz|60 Hz|Norm|Fernsehnorm|TV standard|Region"},
    {Kind::Section, kTabSource, "crossbar", "Eingang der Karte", "Card input",
     "Crossbar|Anschluss|Connector|Buchse|Port"},
    {Kind::Section, kTabSource, "srcaudio", "Ton der Quelle", "Source audio",
     "Audioquelle|Audio source|Ton|Sound"},
    {Kind::Control, kTabSource, "embedded", "Eingebettetes Audio des Videogeräts verwenden",
     "Use the video device's embedded audio", "HDMI Audio|Embedded|Ton über HDMI"},
    {Kind::Control, kTabSource, "audiodev", "Audiogerät", "Audio device",
     "Line-In|Line in|Soundkarte|Sound card|Aufnahmegerät|Audioeingang|Audio input"},
    {Kind::Section, kTabSource, "format", "Format", "Format", "Videoformat|Modus|Mode"},
    {Kind::Control, kTabSource, "subtype", "Farbformat", "Colour format",
     "Color format|YUY2|UYVY|NV12|P010|RGB24|MJPEG|Pixelformat|Pixel format|FourCC|Subtype"},
    {Kind::Control, kTabSource, "resolution", "Auflösung", "Resolution",
     "1080p|720p|576p|480p|1920x1080|4K|Größe|Size|Pixel"},
    {Kind::Control, kTabSource, "fps", "Bildrate", "Frame rate",
     "FPS|Framerate|Hz|Bilder pro Sekunde|Frames per second|50fps|60fps"},
    {Kind::Control, kTabSource, "manualfmt", "Werte von Hand eingeben", "Enter values manually",
     "Custom|Benutzerdefiniert|eigene Auflösung|Custom resolution"},

    // ---- picture
    {Kind::Tab, kTabPicture, nullptr, "Bild", "Picture", "Video|Image|Bildqualität"},
    {Kind::Section, kTabPicture, "scaling", "Skalierung", "Scaling",
     "Upscale|Upscaling|Hochskalieren|Scaler|Resize|Vergrößern"},
    {Kind::Control, kTabPicture, "filter", "Filter", "Filter",
     "Nearest Neighbor|Bilinear|Bicubic|Bikubisch|Lanczos|Sharp Bilinear|Interpolation|"
     "Upscale|Upscaling|Pixel perfect|pixelig|Pixelated|verschwommen|Blurry|unscharf"},
    {Kind::Control, kTabPicture, "sharpen", "Schärfen", "Sharpen",
     "Schärfe|Sharpness|scharf|Sharp|unscharf|Blurry|Details"},
    {Kind::Control, kTabPicture, "aspect", "Seitenverhältnis", "Aspect ratio",
     "16:9|4:3|Stretch|Strecken|Zoom|Integer|Ganzzahlig|Pixel perfect|Quadratische Pixel|"
     "Square pixels|Breitbild|Widescreen|Letterbox|schwarze Balken|Black bars|Füllen|Fill"},
    {Kind::Control, kTabPicture, "aspectout", "Auch für Aufnahme, Screenshot und Kamera",
     "Apply to recording, screenshots and camera",
     "Seitenverhältnis|Aspect ratio|Quadratische Pixel|Square pixels"},
    {Kind::Control, kTabPicture, "rotation", "Drehung", "Rotation",
     "Rotate|Drehen|90 Grad|180 Grad|Hochkant|Portrait|Vertikal|Vertical|Tate|kopfüber|Upside down"},
    {Kind::Control, kTabPicture, "linedouble", "Zeilen verdoppeln", "Double lines",
     "Line doubling|Line doubler|Zeilenverdopplung|240p|288p"},
    {Kind::Section, kTabPicture, "procamp", "Bildregler", "Picture controls",
     "ProcAmp|Bildeinstellungen|Picture settings|Farben|Colours"},
    {Kind::Control, kTabPicture, "brightness", "Helligkeit", "Brightness",
     "Gamma|hell|dunkel|Bright|Dark|Schwarzwert|Black level"},
    {Kind::Control, kTabPicture, "contrast", "Kontrast", "Contrast", "Gamma|Dynamik|flau|Flat"},
    {Kind::Control, kTabPicture, "saturation", "Sättigung", "Saturation",
     "Farbsättigung|Farbe|Farben|Colour|Color|Vibrance|blass|grell|bunt|Graustufen|Grayscale"},
    {Kind::Control, kTabPicture, "hue", "Farbton", "Hue",
     "Tint|Farbstich|Colour cast|Tönung|grünstichig|Greenish|lila|Purple"},
    {Kind::Control, kTabPicture, "procampreset", "Zurücksetzen", "Reset",
     "Bildregler zurücksetzen|Standard|Default"},
    {Kind::Control, kTabPicture, "procampout", "Auch für Aufnahme, Screenshot und Kamera",
     "Apply to recording, screenshots and camera",
     "Bildregler|Picture controls|Helligkeit|Brightness|Kontrast|Contrast"},
    {Kind::Section, kTabPicture, "fields", "Halbbilder", "Fields",
     "Interlacing|Interlaced|Zeilensprung|Halbbild|Field|480i|576i|1080i"},
    {Kind::Control, kTabPicture, "interlacedonly", "Nur bei interlaced Quellen anwenden",
     "Only apply to interlaced sources", "Progressiv|Progressive|240p"},
    {Kind::Control, kTabPicture, "deinterlace", "Deinterlacing", "Deinterlacing",
     "Deinterlace|Weave|Bob|YADIF|Bewegungsadaptiv|Motion adaptive|Kantenorientiert|"
     "Edge directed|Kämme|Kammeffekt|Combing|Zeilensprung|Interlace|Interlacing|flimmern|Flicker"},
    {Kind::Control, kTabPicture, "fieldorder", "Halbbildreihenfolge", "Field order",
     "TFF|BFF|Top field first|Bottom field first|Feldreihenfolge|ruckelt|Judder|zittert"},
    {Kind::Section, kTabPicture, "crop", "Bildrand abschneiden", "Crop",
     "Overscan|Rand|Ränder|Border|schwarze Balken|Black bars|Beschneiden|Zuschneiden|Trim"},
    {Kind::Control, kTabPicture, "cropdetect", "Erkennen", "Detect",
     "Rand erkennen|Autocrop|Auto crop|automatisch zuschneiden"},
    {Kind::Control, kTabPicture, "cropperres", "Je Bildgröße getrennt merken",
     "Remember per picture size", "pro Auflösung|Per resolution|Zuschnitt merken"},
    {Kind::Section, kTabPicture, "native", "Natives Pixelraster", "Native pixel grid",
     "Pixelraster|Pixel grid|native Auflösung|Native resolution|Originalauflösung|Pixel perfect"},
    {Kind::Control, kTabPicture, "nativewidth", "Breite der Quelle", "Source width",
     "SNES|Super Nintendo|NES|PS1|PlayStation|PSX|Mega Drive|Genesis|Amiga|GameCube|PS2|"
     "Dreamcast|N64|256|320|512|640|Konsole|Console|horizontale Auflösung"},
    {Kind::Section, kTabPicture, "composite", "Rauschen", "Noise",
     "Denoise|Entrauschen|Rauschfilter|Noise reduction|Grieseln|Grain|Körnung"},
    {Kind::Section, kTabPicture, "composite", "Composite-Filter", "Composite filter",
     "Composite|FBAS|Cinch|Dot crawl|Punktkriechen|Regenbogen|Rainbow|Artefakte|Artifacts"},
    {Kind::Control, kTabPicture, "chromasoft", "Farbschimmern", "Colour shimmer",
     "Chroma|Farbrauschen|Chroma noise|Regenbogen|Rainbow|Farbsaum|Colour bleed|Color bleed|"
     "Dot crawl|Schimmern|Shimmer"},
    {Kind::Control, kTabPicture, "chromaadaptive", "Nur wo nötig", "Only where needed",
     "Adaptiv|Adaptive|Farbschimmern"},
    {Kind::Control, kTabPicture, "average", "Stillstehendes mitteln", "Average what stands still",
     "Denoise|Entrauschen|Rauschen|Noise|Rauschfilter|Temporal|Grieseln|Grain|Körnung"},
    {Kind::Control, kTabPicture, "ghosting", "Ghosting vermeiden", "Avoid ghosting",
     "Geisterbild|Schlieren|Nachziehen|Trails|Smear|Smearing"},
    {Kind::Control, kTabPicture, "follow", "Bewegung folgen", "Follow the movement",
     "Bewegungskompensation|Motion compensation|Motion"},
    {Kind::Control, kTabPicture, "cleanmove", "Bewegtes entstören", "Clean up what moves",
     "Dot crawl|Punktkriechen|Regenbogen|Rainbow|Kantenflimmern|Entstören|Motion noise"},
    {Kind::Control, kTabPicture, "bandwidth", "Bandbreite zurückholen", "Restore bandwidth",
     "Schärfe|Sharpness|unscharf|Blurry|verwaschen|Details|Luma"},
    {Kind::Section, kTabPicture, "crt", "Bildröhre", "Cathode ray tube",
     "CRT|Röhre|Röhrenfernseher|Retro|Shader|TV-Look|Fernseher|Tube|Trinitron"},
    {Kind::Control, kTabPicture, "crtlines", "Zeilen der Quelle", "Source lines",
     "240p|288p|480i|576i|Zeilenzahl|Line count|vertikale Auflösung|Vertical resolution"},
    {Kind::Control, kTabPicture, "scanlines", "Zeilenlücken", "Scanlines",
     "Scanline|Zeilen|Streifen|Stripes|CRT|Retro|Röhre"},
    {Kind::Control, kTabPicture, "mask", "Maske", "Mask",
     "Streifenmaske|Aperture grille|Trinitron|Lochmaske|Shadow mask|Slot mask|Phosphor|"
     "Subpixel|CRT"},
    {Kind::Control, kTabPicture, "maskstrength", "Maskenstärke", "Mask strength",
     "Phosphor|Intensität|Intensity|CRT"},
    {Kind::Place, kTabPicture, nullptr, "Vergleich", "Compare", nullptr},
    {Kind::Control, kTabPicture, "compare", "Mit und ohne Filter vergleichen",
     "Compare with and without filters",
     "Vergleich|Vorher nachher|Before after|A/B|Split screen|Bypass|Filter aus"},
    {Kind::Control, kTabPicture, "comparedir", "Richtung", "Direction",
     "Vergleich|Horizontal|Vertikal|Vertical"},
    {Kind::Control, kTabPicture, "comparesplit", "Trennlinie", "Divider",
     "Vergleich|Split|Teilung|Position"},
    {Kind::Place, kTabPicture, nullptr, "Farbe", "Colour", nullptr},
    {Kind::Control, kTabPicture, "range", "Wertebereich", "Range",
     "Farbraum|Limited|Full|16-235|0-255|TV range|PC range|RGB range|Schwarzwert|Black level|"
     "verwaschen|Washed out|grau|Crushed blacks|Levels"},
    {Kind::Control, kTabPicture, "matrix", "Farbmatrix", "Colour matrix",
     "Color matrix|BT.601|BT.709|Rec.601|Rec.709|Farbraum|Colour space|Color space|YCbCr|"
     "falsche Farben|Wrong colours"},

    // ---- hdr
    {Kind::Tab, kTabHdr, nullptr, "HDR", "HDR",
     "High dynamic range|Kontrastumfang|HDR10|PQ|HLG|Nits|Tonemapping|Tone mapping"},
    {Kind::Place, kTabHdr, nullptr, "Hoher Kontrastumfang (HDR)", "High dynamic range", nullptr},
    {Kind::Control, kTabHdr, "hdrin", "Quellkurve", "Source curve",
     "PQ|HLG|HDR10|Transferfunktion|Transfer function|EOTF|SDR|Gamma"},
    {Kind::Control, kTabHdr, "hdrout", "An die Anzeige", "To the display",
     "Tonemapping|Tone mapping|scRGB|HDR-Ausgabe|HDR output|Monitor|Bildschirm"},
    {Kind::Control, kTabHdr, "paperwhite", "Papierweiß", "Paper white",
     "Nits|Helligkeit|Brightness|SDR-Helligkeit|SDR brightness|Weißpunkt|White level"},
    {Kind::Control, kTabHdr, "sourcepeak", "Spitze der Quelle", "Source peak",
     "Peak|Nits|Spitzenhelligkeit|Peak brightness|MaxCLL|Mastering"},
    {Kind::Place, kTabHdr, nullptr, "Was den Umfang behält", "What keeps the range", nullptr},
    {Kind::Control, kTabHdr, "hdrrec", "Aufnahme", "Recording",
     "HDR-Aufnahme|HDR recording|10 bit|10-bit|Main10"},
    {Kind::Control, kTabHdr, "hdrshot", "Screenshots", "Screenshots",
     "HDR-Screenshot|JPEG XR|JXR|AVIF"},
    {Kind::Control, kTabHdr, "hdrcam", "Virtuelle Kamera", "Virtual camera", "HDR-Kamera"},

    // ---- audio
    {Kind::Tab, kTabAudio, nullptr, "Ton", "Audio", "Sound|Audio|Lautsprecher|Speaker"},
    {Kind::Section, kTabAudio, "audioout", "Wiedergabe", "Playback",
     "Ausgabegerät|Output device|Lautsprecher|Speakers|Kopfhörer|Headphones|Headset|"
     "Soundkarte|Sound card|Audioausgabe|Audio output"},
    {Kind::Control, kTabAudio, "exclusive", "Exclusive Mode", "Exclusive Mode",
     "Exklusiv|Exklusiver Modus|WASAPI|Latenz|Latency"},
    {Kind::Section, kTabAudio, "delay", "Verzögerung", "Delay",
     "Latenz|Latency|Lipsync|Sync|Versatz|Offset|Lag"},
    {Kind::Control, kTabAudio, "audiobuffer", "Tonpuffer", "Audio buffer",
     "Puffer|Buffer|Latenz|Latency|Knacken|Crackling|Aussetzer|Dropouts|Stottern|Stutter|Knistern"},
    {Kind::Control, kTabAudio, "avoffset", "A/V-Versatz", "A/V offset",
     "Lipsync|Lippensynchron|Sync|Synchron|Asynchron|Out of sync|Delay|Verzögerung|Offset|"
     "Latenz|Audio delay|Ton verzögert|Ton zu früh"},
    {Kind::Place, kTabAudio, nullptr, "Lautstärke", "Volume", nullptr},
    {Kind::Control, kTabAudio, "volume", "Lautstärke", "Volume",
     "laut|leise|Loud|Quiet|Pegel|Level"},
    {Kind::Control, kTabAudio, "mute", "Stumm", "Muted",
     "Mute|Ton aus|Stummschalten|Sound off"},
    {Kind::Place, kTabAudio, nullptr, "Mikrofon", "Microphone", nullptr},
    {Kind::Control, kTabAudio, "mic", "Mikrofon aufnehmen", "Record a microphone",
     "Mic|Mikro|Micro|Kommentar|Commentary|Voice|Stimme|Headset|Facecam"},
    {Kind::Control, kTabAudio, "micdev", "Mikrofon", "Microphone",
     "Mic|Mikro|Headset|Eingabegerät|Input device"},
    {Kind::Control, kTabAudio, "micgain", "Verstärkung", "Gain",
     "Pegel|Level|Mikrofonlautstärke|Mic volume|Boost|dB|laut|leise"},
    {Kind::Control, kTabAudio, "mictracks", "Spuren", "Tracks",
     "Tonspur|Audiospur|Audio track|getrennte Spur|Separate track|Mehrspur|Multitrack"},

    // ---- display
    {Kind::Tab, kTabDisplay, nullptr, "Anzeige", "Display",
     "Oberfläche|Interface|UI|Darstellung|Aussehen|Look"},
    {Kind::Place, kTabDisplay, nullptr, "Sprache", "Language", nullptr},
    {Kind::Control, kTabDisplay, "language", "Sprache", "Language",
     "Deutsch|German|English|Englisch|Übersetzung|Translation"},
    {Kind::Place, kTabDisplay, nullptr, "Fenster", "Window", nullptr},
    {Kind::Control, kTabDisplay, "ownwindow", "Einstellungen in eigenem Fenster",
     "Settings in their own window",
     "Separates Fenster|Separate window|Abdocken|Undock|Popup"},
    {Kind::Place, kTabDisplay, nullptr, "Darstellung", "Appearance", nullptr},
    {Kind::Control, kTabDisplay, "theme", "Design", "Theme",
     "Dark mode|Dunkel|Hell|Light|Dark|Farbschema|Colour scheme|Nachtmodus|Look"},
    {Kind::Control, kTabDisplay, "accent", "Akzentfarbe", "Accent colour",
     "Accent color|Farbe|Colour|Color|Akzent|Highlight"},
    {Kind::Control, kTabDisplay, "customcolour", "Eigene Farbe", "Custom colour",
     "Custom color|Hex|RGB|Farbwähler|Colour picker|Color picker"},
    {Kind::Place, kTabDisplay, nullptr, "Verhalten", "Behaviour", nullptr},
    {Kind::Control, kTabDisplay, "vsync", "VSync", "VSync",
     "Tearing|Bildriss|V-Sync|Vertical sync|Vertikale Synchronisation|G-Sync|FreeSync|Lag|"
     "Input lag|Eingabeverzögerung"},
    {Kind::Control, kTabDisplay, "ontop", "Immer im Vordergrund", "Always on top",
     "Topmost|Oben halten|Pin|Anheften"},
    {Kind::Control, kTabDisplay, "borderless", "Rahmenlos", "Borderless",
     "Ohne Rahmen|Frameless|Titelleiste|Title bar|Randlos"},
    {Kind::Control, kTabDisplay, "hidecursor", "Mauszeiger im Vollbild ausblenden",
     "Hide cursor in fullscreen", "Cursor|Maus|Mouse|Zeiger|Pointer"},
    {Kind::Control, kTabDisplay, "nosleep", "Bildschirmschoner und Standby verhindern",
     "Prevent screensaver and sleep",
     "Standby|Sleep|Energiesparen|Power saving|Ruhezustand|Monitor aus|Display off|"
     "wach halten|Keep awake"},
    {Kind::Control, kTabDisplay, "toolbar", "Werkzeugleiste anzeigen", "Show toolbar",
     "Toolbar|Leiste|Buttons|Knöpfe|Symbolleiste|Menüleiste"},
    {Kind::Control, kTabDisplay, "stats", "Statistik einblenden", "Show statistics",
     "Stats|FPS-Anzeige|FPS counter|OSD|Overlay|Info|Dropped frames|verlorene Bilder"},
    {Kind::Control, kTabDisplay, "statsdetail", "Umfang", "Detail",
     "Statistik|Statistics|Details|ausführlich|Verbose"},
    {Kind::Section, kTabDisplay, "volosdsec", "Lautstärke-Anzeige", "Volume readout",
     "Lautstärke-OSD|Volume OSD|Einblendung|Overlay"},
    {Kind::Control, kTabDisplay, "volosd", "Bei Änderung einblenden", "Show on change",
     "OSD|Lautstärke|Volume|Overlay|Einblendung"},
    {Kind::Control, kTabDisplay, "osdcorner", "Ecke", "Corner",
     "Position|oben|unten|links|rechts|Top|Bottom|Left|Right"},
    {Kind::Control, kTabDisplay, "wheelvolume", "Mausrad über dem Bild ändert die Lautstärke",
     "Mouse wheel over the picture changes the volume", "Scrollrad|Scroll|Wheel"},
    {Kind::Section, kTabDisplay, "fsmonitor", "Vollbild", "Fullscreen",
     "Monitor|Bildschirm|Screen|zweiter Monitor|Second monitor|Display"},
    {Kind::Control, kTabDisplay, "startfs", "Beim Start im Vollbild öffnen", "Start in fullscreen",
     "Autostart|On startup|Kiosk"},
    {Kind::Section, kTabDisplay, "traysec", "Infobereich", "Notification area",
     "Tray|Systray|System tray|Taskleiste|Taskbar|Uhr|Clock"},
    {Kind::Control, kTabDisplay, "tray", "Symbol im Infobereich", "Icon in the notification area",
     "Tray-Icon|Tray icon|Systray|Infobereich|Notification area|Symbol|Icon"},
    {Kind::Control, kTabDisplay, "trayitems", "Im Menü des Symbols", "In the icon's menu",
     "Schnellzugriff|Quick actions|Schnellaktionen|Tray-Menü|Tray menu|Befehle|Commands"},
    {Kind::Place, kTabDisplay, nullptr, "Sonstiges", "Other", nullptr},
    {Kind::Control, kTabDisplay, "log", "Protokoll schreiben", "Write a log",
     "Log|Logfile|Logdatei|Debug|Fehlersuche|Troubleshooting|Diagnose"},
    {Kind::Control, kTabDisplay, "logretention", "Alte Protokolle entfernen", "Remove old logs",
     "Log|Aufräumen|Cleanup|Sitzungen|Sessions|Retention"},

    // ---- recording
    {Kind::Tab, kTabRecord, nullptr, "Aufnahme", "Recording",
     "Record|Aufnehmen|Mitschnitt|Video speichern|Capture|REC"},
    {Kind::Place, kTabRecord, nullptr, "Ausgabe", "Output", nullptr},
    {Kind::Control, kTabRecord, "container", "Container", "Container",
     "MKV|MP4|Matroska|Dateiformat|File format|Dateityp|Format"},
    {Kind::Control, kTabRecord, "recfps", "Bildrate", "Frame rate",
     "FPS|Framerate|30fps|60fps|Aufnahme-Bildrate|Recording frame rate"},
    {Kind::Control, kTabRecord, "recfolder", "Aufnahmeordner", "Recording folder",
     "Speicherort|Ordner|Pfad|Folder|Path|Location|Verzeichnis|Directory|Zielordner|Save location"},
    {Kind::Control, kTabRecord, "split", "Bei Größe aufteilen", "Split at size",
     "FAT32|4 GB|Teilen|Splitten|Dateigröße|File size|Segment"},
    {Kind::Control, kTabRecord, "splitsize", "Teilgröße", "Split size",
     "MB|GB|Dateigröße|File size|Segmentgröße"},
    {Kind::Section, kTabRecord, "shots", "Screenshots", "Screenshots",
     "Bildschirmfoto|Screenshot|Snapshot|Standbild|Still|Foto|Photo|Bild speichern"},
    {Kind::Control, kTabRecord, "shotui", "Bedienoberfläche mit aufnehmen", "Include the interface",
     "UI|Overlay|OSD|Oberfläche|Menü|Interface"},
    {Kind::Control, kTabRecord, "shotformat", "Format", "Format",
     "PNG|JPEG|JPG|Dateiformat|File format|Bildformat|Screenshot"},
    {Kind::Control, kTabRecord, "jpegq", "JPEG-Qualität", "JPEG quality",
     "JPG|Kompression|Compression|Qualität|Quality"},
    {Kind::Control, kTabRecord, "shotfolder", "Screenshot-Ordner", "Screenshot folder",
     "Speicherort|Ordner|Pfad|Folder|Path|Location|Verzeichnis|Directory"},
    {Kind::Section, kTabRecord, "vcam", "Virtuelle Kamera", "Virtual camera",
     "Webcam|OBS|Discord|Zoom|Teams|Skype|Streaming|Stream|Twitch|Kamera|Camera|DirectShow"},
    {Kind::Control, kTabRecord, "vcaminstall", "Kamera installieren", "Install camera",
     "Installieren|Registrieren|Register|Einrichten|Setup|Webcam"},
    {Kind::Control, kTabRecord, "vcamon", "Virtuelle Kamera einschalten",
     "Turn the virtual camera on", "Webcam|OBS|Discord|aktivieren|Enable|Start"},
    {Kind::Control, kTabRecord, "vcamuninstall", "Kamera deinstallieren", "Uninstall camera",
     "Entfernen|Remove|Deregistrieren|Webcam"},
    {Kind::Section, kTabRecord, "remux", "Nach MP4 umpacken", "Rewrap to MP4",
     "Remux|Remuxen|Umwandeln|Convert|Konvertieren|MKV zu MP4|MKV to MP4|Container"},

    // ---- encoder
    {Kind::Tab, kTabEncoder, nullptr, "Encoder", "Encoder",
     "Codec|Kodierung|Encoding|Kompression|Compression|Hardware|GPU"},
    {Kind::Section, kTabEncoder, "ffmpeg", "ffmpeg", "ffmpeg",
     "FFmpeg|Download|Herunterladen|Encoder fehlt|Missing"},
    {Kind::Control, kTabEncoder, "ffmpegpath", "Eigener Pfad zu ffmpeg.exe",
     "Custom path to ffmpeg.exe", "Pfad|Path|Eigene ffmpeg|Custom ffmpeg"},
    {Kind::Place, kTabEncoder, nullptr, "Encoder", "Encoder", nullptr},
    {Kind::Control, kTabEncoder, "encoder", "Encoder", "Encoder",
     "Codec|H.264|H264|AVC|H.265|H265|HEVC|AV1|NVENC|NVIDIA|QuickSync|QSV|Intel|AMF|AMD|"
     "x264|x265|SVT|Hardware|GPU|CPU"},
    {Kind::Control, kTabEncoder, "encodertest", "Encoder testen", "Test encoders",
     "Test|Prüfen|Check|Probe"},
    {Kind::Control, kTabEncoder, "speed", "Geschwindigkeit", "Speed",
     "Preset|Ultrafast|Veryfast|Slow|CPU-Last|CPU load|schnell|langsam"},
    {Kind::Section, kTabEncoder, "encsettings", "Encoder-Einstellungen", "Encoder settings",
     "Codec-Einstellungen|Codec settings"},
    {Kind::Control, kTabEncoder, "ratecontrol", "Ratensteuerung", "Rate control",
     "Bitrate|Datenrate|CBR|VBR|CQP|konstante Qualität|Constant quality|Bitratenmodus"},
    {Kind::Control, kTabEncoder, "bitrate", "Bitrate", "Bitrate",
     "kbps|kbit|Mbps|Mbit|Datenrate|Data rate|Dateigröße|File size"},
    {Kind::Control, kTabEncoder, "quality", "Qualität", "Quality",
     "CRF|CQ|QP|Qualitätsstufe|Quality level|konstante Qualität"},
    {Kind::Control, kTabEncoder, "preset", "Voreinstellung", "Preset",
     "Preset|P1|P7|Geschwindigkeit|Speed"},
    {Kind::Control, kTabEncoder, "tune", "Abstimmung", "Tuning",
     "Tune|geringe Latenz|Low latency|Zerolatency|Film|Animation"},
    {Kind::Control, kTabEncoder, "multipass", "Durchläufe", "Multipass",
     "2-Pass|Two pass|Zwei Durchläufe|Pass"},
    {Kind::Control, kTabEncoder, "lookahead", "Vorausschau", "Look-ahead",
     "Lookahead|RC lookahead|B-Frames"},
    {Kind::Control, kTabEncoder, "aq", "Adaptive Quantisierung", "Adaptive quantisation",
     "AQ|Spatial AQ|Temporal AQ|Quantisierung|Quantization"},

    // ---- keys
    {Kind::Tab, kTabKeys, nullptr, "Tasten", "Keys",
     "Hotkey|Hotkeys|Shortcut|Shortcuts|Tastenkürzel|Tastenkombination|Tastatur|Keyboard|"
     "Belegung|Binding|Kürzel"},
    {Kind::Control, kTabKeys, "keysreset", "Alle zurücksetzen", "Reset all",
     "Tasten zurücksetzen|Standardbelegung|Default keys|Hotkeys"},

    // ---- profiles
    {Kind::Tab, kTabProfiles, nullptr, "Profile", "Profiles",
     "Profil|Konsole|Console|Voreinstellungen|Presets|Konfiguration|Setup|Einstellungen speichern"},
    {Kind::Control, kTabProfiles, "profnew", "Neues Profil", "New profile", "Neu|New|Anlegen"},
    {Kind::Control, kTabProfiles, "profsave", "Aktuelles sichern als", "Save current as",
     "Speichern|Save|Kopie|Copy|Duplizieren|Duplicate|Profil"},
    {Kind::Control, kTabProfiles, "profrename", "Umbenennen", "Rename", "Profil|Name"},
    {Kind::Control, kTabProfiles, "profdelete", "Löschen", "Delete", "Profil|Entfernen|Remove"},
    {Kind::Section, kTabProfiles, "autoselect", "Von selbst wählen", "Choose automatically",
     "Automatisch|Automatic|Auto|Profilwechsel|Switch profile|Norm erkennen"},

    // ---- updates
    {Kind::Tab, kTabUpdates, nullptr, "Updates", "Updates",
     "Update|Aktualisierung|Version|neue Version|New version|Changelog|Release|Was neu ist|"
     "What is new|Neuigkeiten"},
    {Kind::Place, kTabUpdates, nullptr, "Version", "Version", nullptr},
    {Kind::Control, kTabUpdates, "updatestartup", "Beim Start nach Updates suchen",
     "Check for updates at startup", "Automatisch|Automatic|Autoupdate|Update-Check"},
    {Kind::Place, kTabUpdates, nullptr, "Stand", "Status", nullptr},
    {Kind::Control, kTabUpdates, "updatecheck", "Jetzt suchen", "Check now",
     "Update suchen|Check for updates|nach Updates suchen"},
};

constexpr int kEntryCount = (int)(sizeof(kEntries) / sizeof(kEntries[0]));

// ------------------------------------------------------------ normalisation

// Beide Seiten gehen hier durch, deshalb darf es grob sein: was aus "Größe"
// wird, ist egal, solange "groesse", "grosse" und "Groesse" dasselbe werden.
// Punkt, Schraegstrich und Apostroph verbinden statt zu trennen -- "H.264",
// "A/V" und "BT.601" sind je ein Wort, und so tippt man sie auch.
std::string Normalize(const char* s) {
  std::string out;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
    const unsigned char c = *p;
    if (c == 0xC3 && p[1]) {
      switch (p[1]) {
        case 0xA4: case 0x84: out += 'a'; break;   // ä Ä
        case 0xB6: case 0x96: out += 'o'; break;   // ö Ö
        case 0xBC: case 0x9C: out += 'u'; break;   // ü Ü
        case 0x9F: out += "ss"; break;             // ß
        case 0xA9: case 0x89: case 0xA8: out += 'e'; break;  // é É è
        default: out += ' '; break;
      }
      ++p;
      continue;
    }
    if (c >= 0xC0) {  // the lead byte of anything else
      out += ' ';
      continue;
    }
    if (c >= 0x80) continue;  // its continuation bytes
    if (c >= 'A' && c <= 'Z') {
      out += (char)(c - 'A' + 'a');
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out += (char)c;
    } else if (c == '/' || c == '.' || c == '\'') {
      // joins
    } else {
      out += ' ';
    }
  }
  // ae, oe, ue -> a, o, u: whoever cannot type an umlaut writes it this way.
  std::string folded;
  folded.reserve(out.size());
  for (size_t i = 0; i < out.size(); ++i) {
    folded += out[i];
    if ((out[i] == 'a' || out[i] == 'o' || out[i] == 'u') && i + 1 < out.size() &&
        out[i + 1] == 'e') {
      ++i;
    }
  }
  return folded;
}

std::vector<std::string> Words(const std::string& s) {
  std::vector<std::string> words;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    if (j > i) words.push_back(s.substr(i, j - i));
    i = j;
  }
  return words;
}

// One label or one synonym phrase, taken apart once.
struct Field {
  std::vector<std::string> words;
  std::string compact;          // the words without the spaces
  std::vector<size_t> starts;   // where each word begins in it
};

Field MakeField(const char* text) {
  Field f;
  f.words = Words(Normalize(text));
  for (const std::string& w : f.words) {
    f.starts.push_back(f.compact.size());
    f.compact += w;
  }
  return f;
}

struct Prepared {
  Field de, en;
  std::vector<Field> syn;
  std::vector<Field> place;  // tab and section, both languages
  int section = -1;          // the section entry it sits under
  int tabEntry = -1;         // the tab entry it sits under
};

const std::vector<Prepared>& Index() {
  static const std::vector<Prepared> index = [] {
    std::vector<Prepared> all(kEntryCount);
    int tabEntry = -1, section = -1;
    for (int i = 0; i < kEntryCount; ++i) {
      const Entry& e = kEntries[i];
      Prepared& p = all[i];
      p.de = MakeField(e.de);
      p.en = MakeField(e.en);
      if (e.syn) {
        const char* s = e.syn;
        while (*s) {
          const char* bar = s;
          while (*bar && *bar != '|') ++bar;
          p.syn.push_back(MakeField(std::string(s, bar).c_str()));
          s = *bar ? bar + 1 : bar;
        }
      }
      if (e.kind == Kind::Tab) {
        tabEntry = i;
        section = -1;
        continue;
      }
      if (e.kind == Kind::Section || e.kind == Kind::Place) section = i;
      p.tabEntry = tabEntry;
      p.section = (e.kind == Kind::Control) ? section : -1;
      for (int owner : {p.tabEntry, p.section}) {
        if (owner < 0) continue;
        p.place.push_back(MakeField(kEntries[owner].de));
        p.place.push_back(MakeField(kEntries[owner].en));
      }
    }
    return all;
  }();
  return index;
}

// ------------------------------------------------------------------ matching

// Optimal string alignment: Levenshtein plus swapping two neighbours, which is
// the typo people actually make ("Kontarst"). Gives up past the limit.
int Distance(const std::string& a, const std::string& b, int limit) {
  const int n = (int)a.size(), m = (int)b.size();
  if (std::abs(n - m) > limit) return limit + 1;
  std::vector<int> prev2(m + 1), prev(m + 1), cur(m + 1);
  for (int j = 0; j <= m; ++j) prev[j] = j;
  for (int i = 1; i <= n; ++i) {
    cur[0] = i;
    int rowMin = cur[0];
    for (int j = 1; j <= m; ++j) {
      const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      int d = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
        d = std::min(d, prev2[j - 2] + 1);
      }
      cur[j] = d;
      rowMin = std::min(rowMin, d);
    }
    if (rowMin > limit) return limit + 1;
    prev2.swap(prev);
    prev.swap(cur);
  }
  return prev[m];
}

// Against the whole word and against its beginnings around the query's length,
// so a typo in a word still being typed finds it too ("Helig" -> "Helligkeit").
int Typo(const std::string& q, const std::string& w) {
  if (q.size() < 4) return -1;
  const int limit = q.size() >= 7 ? 2 : 1;
  int best = Distance(q, w, limit);
  for (size_t len = q.size() - 1; len <= q.size() + 1; ++len) {
    if (len < w.size()) best = std::min(best, Distance(q, w.substr(0, len), limit));
  }
  return best <= limit ? best : -1;
}

// The letters in order, with gaps: "hlgkt" -> "helligkeit". Only as a last
// resort, and not into words so long that almost anything fits.
bool Subsequence(const std::string& q, const std::string& w) {
  if (q.size() < 4 || q[0] != w[0] || w.size() > q.size() * 2 + 2) return false;
  size_t i = 0;
  for (char c : w) {
    if (c == q[i] && ++i == q.size()) return true;
  }
  return false;
}

// Somewhere inside a word -- the back half of a compound ("rand" in
// "Bildrand"). Short pieces only at the end: in the middle they turn up in
// words that have nothing to do with them ("crop" in "microphone").
bool Inside(const std::string& q, const std::string& w) {
  if (q.size() < 4) return false;
  const size_t at = w.find(q);
  if (at == std::string::npos) return false;
  return q.size() >= 5 || at + q.size() == w.size();
}

// 100 exact, 80 the beginning of a word (or of several run together), 60
// somewhere inside, 30/20 a typo, 20 the letters in order. -1 for nothing.
int MatchWord(const std::string& q, const Field& f) {
  int best = -1;
  for (size_t i = 0; i < f.words.size() && best < 100; ++i) {
    const std::string& w = f.words[i];
    int s = -1;
    if (w == q) {
      s = 100;
    } else if (w.size() > q.size() && w.compare(0, q.size(), q) == 0) {
      s = 80;
    } else if (q.size() >= 2 && f.compact.compare(f.starts[i], q.size(), q) == 0) {
      s = 80;
    } else if (Inside(q, w)) {
      s = 60;
    } else {
      const int d = Typo(q, w);
      if (d >= 0) {
        s = 40 - 10 * d;
      } else if (Subsequence(q, w)) {
        s = 20;
      }
    }
    best = std::max(best, s);
  }
  return best;
}

int MatchAny(const std::string& q, const std::vector<Field>& fields) {
  int best = -1;
  for (const Field& f : fields) best = std::max(best, MatchWord(q, f));
  return best;
}

bool IsStopWord(const std::string& w) {
  static const char* const kStop[] = {"der", "die", "das", "den", "dem", "des", "ein", "eine",
                                      "einen", "und", "oder", "fur", "von", "zu", "im", "in",
                                      "am", "the", "a", "an", "of", "for", "to", "and", "or",
                                      "on"};
  for (const char* s : kStop) {
    if (w == s) return true;
  }
  return false;
}

}  // namespace

std::vector<SettingsSearchHit> SearchSettings(const char* query) {
  std::vector<SettingsSearchHit> hits;
  std::vector<std::string> words = Words(Normalize(query));
  std::vector<std::string> kept;
  for (const std::string& w : words) {
    if (!IsStopWord(w)) kept.push_back(w);
  }
  if (!kept.empty()) words.swap(kept);
  if (words.empty()) return hits;

  const bool english = SpeakingEnglish();
  const std::vector<Prepared>& index = Index();
  for (int i = 0; i < kEntryCount; ++i) {
    if (kEntries[i].kind == Kind::Place) continue;
    const Prepared& p = index[i];
    const Field& shown = english ? p.en : p.de;
    const Field& other = english ? p.de : p.en;

    int total = 0;
    bool direct = true;
    bool own = false;
    bool all = true;
    for (const std::string& q : words) {
      int best = MatchWord(q, shown);
      bool wordDirect = best >= 60;
      const int o = MatchWord(q, other);
      if (o > best) {
        best = o;
        wordDirect = false;
      }
      const int s = MatchAny(q, p.syn);
      if (s >= 0 && s - 5 > best) {
        best = s - 5;
        wordDirect = false;
      }
      if (best >= 0) {
        own = true;
        direct = direct && wordDirect;
        total += best;
        continue;
      }
      // Where it sits narrows the search ("Mikrofon Verstärkung") but never
      // finds anything on its own -- every control in a section would match.
      const int at = MatchAny(q, p.place);
      if (at < 60) {
        all = false;
        break;
      }
      total += at - 30;
    }
    if (!all || !own) continue;

    const Kind kind = kEntries[i].kind;
    total += kind == Kind::Tab ? 4 : kind == Kind::Section ? 2 : 0;
    hits.push_back({i, total, direct});
  }

  std::stable_sort(hits.begin(), hits.end(),
                   [](const SettingsSearchHit& a, const SettingsSearchHit& b) {
                     if (a.score != b.score) return a.score > b.score;
                     return a.direct && !b.direct;
                   });
  // A guess far behind the best hit is noise: letters that happen to fit.
  if (!hits.empty()) {
    const int floor = hits.front().score / 2;
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [floor](const SettingsSearchHit& h) {
                                return !h.direct && h.score < floor;
                              }),
               hits.end());
  }
  return hits;
}

const char* SearchLabel(int entry) {
  if (entry < 0 || entry >= kEntryCount) return "";
  return T(kEntries[entry].de, kEntries[entry].en);
}

std::string SearchPlace(int entry) {
  if (entry < 0 || entry >= kEntryCount) return {};
  const Prepared& p = Index()[entry];
  if (kEntries[entry].kind == Kind::Tab) return T("Reiter", "Tab");
  std::string place = SearchLabel(p.tabEntry);
  if (p.section >= 0) {
    place += " \xE2\x80\xBA ";
    place += SearchLabel(p.section);
  }
  return place;
}

const char* SearchKey(int entry) {
  return entry >= 0 && entry < kEntryCount ? kEntries[entry].key : nullptr;
}

int SearchTab(int entry) {
  return entry >= 0 && entry < kEntryCount ? kEntries[entry].tab : kTabSource;
}

}  // namespace cap
