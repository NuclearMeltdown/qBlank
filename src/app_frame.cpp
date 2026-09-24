// The main loop and the frame: pacing, rendering and presenting, and what
// takes the finished picture -- recorder, virtual camera, screenshots, HDR.

#include "app.h"

#include <cmath>

#include "i18n.h"
#include "imgui_internal.h"
#include "record/screenshot.h"
#include "ui/theme.h"

namespace cap {

int App::Run() {
  while (running_) {
    if (!PumpEvents()) running_ = false;
    if (!running_) break;

    Tick();

    if (minimized_) {
      // Nothing to draw; block on messages so we use no CPU at all.
      WaitForEvents();
      continue;
    }

    // The preview is drawn when there is a new picture to draw, and at no
    // other time.
    //
    // This is the second attempt at pacing it and the first was wrong in a way
    // worth recording. Waking on any message and redrawing was clearly wrong --
    // measured, the whole pipeline ran 235 times a second for a source
    // delivering 25. But replacing it with a clock was no better: at a fixed
    // 33 ms against a 25 fps source the two beat against each other, which is
    // judder of exactly the kind the change was meant to remove. A source has a
    // cadence; anything that is not that cadence is wrong.
    //
    // So: the frame event, a second field falling due, and a slow floor that
    // only matters when no pictures are arriving at all -- with a source
    // running, everything on screen animates at the source's rate anyway.
    const int64_t nowQpc = ClockTicks();
    const double sinceRenderMs =
        lastRenderQpc_ == 0 ? 1e9 : TicksToSeconds(nowQpc - lastRenderQpc_) * 1000.0;
    const bool wokeOnPicture = lastWake_ == WaitResult::Signal;
    // Due by the clock, not by *how* the wait ended.
    //
    // This used to read `lastWait_ == WAIT_TIMEOUT`, which is only ever true
    // when nothing else woke the loop at all. Open the settings and there is a
    // steady stream of messages, so the wait returns "input available" every
    // time and the timeout branch never runs -- on an interlaced source that
    // silently dropped every second field for as long as the dialog was open,
    // because the next arriving picture takes the other branch and resets the
    // field index before the second one was ever drawn.
    //
    // The same shape of mistake as the WM_TIMER that could not compete with the
    // drag loop's message flood: a schedule must not be conditional on the
    // queue being quiet.
    const bool fieldDue = secondFieldPending_ && nowQpc >= secondFieldQpc_;
    const double idleFloor = IdleFloorMs();
    if (wokeOnPicture || fieldDue || sinceRenderMs >= idleFloor) {
      lastRenderQpc_ = nowQpc;
      RenderFrame();
    }

    // And the settings window is drawn on its own account, every time round,
    // with its own throttle inside. It wants to follow the mouse; the preview
    // wants to follow the capture card. Tying them together made one of them
    // wrong whichever rate was chosen.
    DrawSettingsWindowed();
    // The icon's menu likewise, while it is open.
    DrawTrayMenu();

    // Wait for the next captured frame, a pending second field, or input.
    //
    // This bounds how long the loop *sleeps*, not how often it draws. It used
    // to be both, back when any wake-up redrew the preview -- 16 rather than 8
    // was the ceiling that stopped the settings dialog from driving the whole
    // video pipeline at a hundred and twenty-five times a second for the sake
    // of feeling responsive. Since the redraw became conditional on a picture
    // having arrived, the ceiling is gone: a source delivering two hundred and
    // forty frames signals the event two hundred and forty times and gets two
    // hundred and forty redraws, and this timeout never comes into it.
    // Short while the dialog is open, because that is what keeps *it* smooth;
    // it no longer costs the preview anything, since a wake-up without a
    // picture no longer redraws the preview.
    // Ein Boden nuetzt nichts, wenn die Schleife laenger schlaeft als er lang
    // ist. Solange die Einstellungen offen sind kurz, weil das freigestellte
    // Fenster jede Runde gezeichnet wird; sonst so kurz, wie der Boden es
    // verlangt, und hoechstens 100 ms.
    int timeout =
        settings_.isOpen() || trayPopup_.isOpen() ? 16 : Clamp((int)idleFloor, 16, 100);
    if (secondFieldPending_) {
      // Rounded up, not truncated. Truncating asks to be woken a fraction of a
      // millisecond before the field is due, at which point the loop finds it is
      // not due yet, redraws the same field for nothing and then spins on a zero
      // timeout until it is. Waiting the extra millisecond costs a millisecond
      // and saves all of that.
      const double waitMs = TicksToSeconds(secondFieldQpc_ - ClockTicks()) * 1000.0;
      timeout = Clamp((int)std::ceil(waitMs), 0, timeout);
    }
    FrameBuffer* sink = capture_.sink();
    lastWake_ = WaitForEventsOr(sink ? &sink->frameReady() : nullptr, timeout);
  }
  return 0;
}

void App::RenderFrame() {
  const int64_t now = ClockTicks();
  const Profile& profile = config_.active();

  // ---- pull the newest frame ----
  FrameBuffer* sink = capture_.sink();
  bool haveNewFrame = false;
  if (sink) {
    FrameView view;
    if (sink->AcquireFrame(&view) && view.valid()) {
      if (!sawFirstFrame_) {
        sawFirstFrame_ = true;
        CAP_LOG("First frame after %.0f ms (%zu bytes)",
                TicksToSeconds(now - captureStartQpc_) * 1000.0, view.size);
      }
      renderer_.SetSourceFormat(sink->format(), nullptr);
      // Das Standbild haelt hier an und nirgends sonst: die Bilder werden
      // weiter abgeholt, damit der Zulauf nicht auflaeuft und die Statistik
      // stimmt, nur in die Textur geht keines mehr. Alles dahinter -- Filter,
      // Skalierung, Regler -- laeuft am stehenden Bild weiter.
      if (frozen_) {
        // nichts hochladen
      } else if (delayLine_.active()) {
        // Mit der Ankunftszeit und nicht mit `now`: die Verzoegerungsleitung
        // soll das Bild um die eingestellte Zeit nach seinem Eintreffen
        // herausgeben, nicht nach dem Zeichendurchgang, der es aufgegriffen hat.
        delayLine_.Push(view, sink->lastArrivalTicks());
      } else {
        renderer_.UploadFrame(view);
        haveNewFrame = true;
        displayedArrivalQpc_ = sink->lastArrivalTicks();
      }
    }
    if (!frozen_ && delayLine_.active()) {
      FrameView delayed;
      if (delayLine_.Pop(&delayed, now)) {
        renderer_.UploadFrame(delayed);
        haveNewFrame = true;
        displayedArrivalQpc_ = delayLine_.lastPoppedQpc();
      }
    }
  }

  // ---- bob deinterlacing: second field of the previous frame ----
  const VideoFormatInfo format = renderer_.sourceFormat();
  const bool deinterlacing =
      profile.image.deinterlace != Deinterlace::Off && SourceLooksInterlaced(profile);

  // Which field is the earlier one. The media type is asked first and is usually
  // silent on an analogue card, in which case top-field-first is the convention
  // for standard definition -- but a wrong guess here does not soften the
  // picture, it makes it jump: the two fields are shown in the wrong order, so
  // every frame steps back half a frame and then forward again. Hence the
  // override in the settings, which is the only reliable fix when the card says
  // nothing.
  int firstField = format.fieldOneFirst ? 0 : 1;
  if (profile.image.fieldOrder == FieldOrder::TopFirst) firstField = 0;
  else if (profile.image.fieldOrder == FieldOrder::BottomFirst) firstField = 1;

  if (haveNewFrame) {
    fieldIndex_ = firstField;
    if (deinterlacing) ++fieldsShown_[0];
    if (deinterlacing) {
      // The announced rate and the delivered rate are not always the same
      // number. This card announces 50 fps and hands over 25 woven frames a
      // second -- half the rate, twice the content per frame. Splitting the
      // announced interval would put the second field on screen 10 ms in and
      // then leave it there for 30, which judders harder than not deinterlacing
      // at all, so the measured arrival rate wins whenever there is one.
      double frameSeconds = format.fps > 1.0 ? 1.0 / format.fps : 1.0 / 60.0;
      const double measured = sink ? sink->stats().sourceFps : 0.0;
      if (measured > 1.0) frameSeconds = 1.0 / measured;

      // Counted from when the frame arrived, not from when we got round to
      // drawing it. Those are not the same instant: the log has shown the
      // displayed frame to be twenty milliseconds old, and half a frame after
      // *that* falls past the arrival of the next one -- at which point the
      // second field is never shown at all. Half the frames then get both
      // fields and half get one, which is exactly what a juddering picture is.
      const int64_t arrival = sink && sink->lastArrivalTicks() != 0 ? sink->lastArrivalTicks() : now;
      const int64_t period = SecondsToTicks(frameSeconds);

      // The card delivers when the driver gets round to it -- the graph runs
      // without a reference clock on purpose -- and measured here the arrivals
      // wander by ten milliseconds either way. Half a frame after each raw
      // arrival therefore lands anywhere, and bob ends up showing one field for
      // five milliseconds and the next for twenty-eight. The average is exactly
      // right and the picture stutters anyway.
      //
      // So the next arrival is predicted from a phase that is nudged towards the
      // arrivals rather than following each one, and the second field is put
      // halfway between the frame we have and the frame we expect. A late frame
      // then shortens both of its fields equally instead of crushing one of them.
      if (framePhaseQpc_ == 0 || std::llabs(arrival - framePhaseQpc_) > period) {
        framePhaseQpc_ = arrival;  // first frame, or the stream jumped
      } else {
        framePhaseQpc_ += (arrival - framePhaseQpc_) / 8;
      }
      const int64_t expectedNext = framePhaseQpc_ + period;
      framePhaseQpc_ = expectedNext;

      int64_t gap = (expectedNext - arrival) / 2;
      const int64_t minGap = period / 4;
      const int64_t maxGap = period * 3 / 4;
      if (gap < minGap) gap = minGap;
      if (gap > maxGap) gap = maxGap;
      secondFieldQpc_ = arrival + gap;
      secondFieldPending_ = true;
    } else {
      secondFieldPending_ = false;
    }
  } else if (secondFieldPending_ && now >= secondFieldQpc_) {
    fieldIndex_ = 1 - firstField;
    secondFieldPending_ = false;
    ++fieldsShown_[1];
  }

  // ---- draw ----
  float clear[4];
  GetBackgroundColor(darkMode_, config_.app.accentColor, clear);
  if (!display_.BeginFrame(clear)) return;

  // The window is on a monitor with other scaling now. Sizes, spacing and font
  // follow it from this frame on.
  if (pendingUiScale_ > 0.0f) {
    if (pendingUiScale_ != uiScale_) {
      uiScale_ = pendingUiScale_;
      ApplyTheme();
    }
    pendingUiScale_ = 0.0f;
  }

  // Decided before drawing, so the picture is laid out around the bar in the
  // same frame the bar appears in.
  const int topInset = toolbarVisible_ ? (int)std::lround(ToolbarHeight()) : 0;
  renderer_.SetTopInset(topInset);
  UpdateHdr();
  renderer_.Draw(EffectiveImage(profile), fieldIndex_);
  // The shape Shift holds while the window is resized.
  window_.SetSizingAspect(renderer_.hasFrame() ? renderer_.pictureAspect() : 0.0, topInset);

  // Right after the first pass, so the still is the picture that was just put on
  // screen -- and before the UI is drawn, so the overlay never lands in it. The
  // other setting takes the same shot one step later; see below.
  if (screenshotPending_ && !config_.record.screenshotIncludeUi) {
    screenshotPending_ = false;
    WriteScreenshot(false, screenshotToClipboard_);
  }

  display_.NewUiFrame();
  window_.BeginUiFrame();
  ImGui::NewFrame();
  DrawUi();
  ImGui::Render();
  // In HDR the interface goes to a buffer of its own first: it is drawn in sRGB
  // and the screen is being fed linear light, so it needs converting rather than
  // copying. In SDR both calls do nothing and it draws straight to the screen.
  const bool uiLayer = renderer_.BeginUiLayer();
  display_.RenderUi(ImGui::GetDrawData());
  if (uiLayer) renderer_.CompositeUiLayer();

  // The other grab point. Everything has been drawn and nothing has been
  // presented yet, which is the only moment the back buffer holds the finished
  // window: under the flip model its contents are undefined after the present.
  if (screenshotPending_) {
    screenshotPending_ = false;
    WriteScreenshot(true, screenshotToClipboard_);
  }

  // Beide Messwerte jedes Bild, unabhaengig davon, ob das Panel offen ist: das
  // Log schreibt seine Statuszeile auch dann, und ein Ruckler waehrend einer
  // geschlossenen Anzeige ist genau der, den man spaeter sucht.
  if (captureState_ == CaptureState::Running) {
    const AudioStats audioNow = audio_.stats();
    if (audioNow.running) audioBufferMeter_.Sample(audioNow.bufferMs, TicksToSeconds(now));
  }
  recording_.SyncMicrophone();
  recording_.FeedRecorder();
  UpdateVirtualCamera();
  FeedFrameConsumers();
  display_.EndFrame(config_.app.vsync);

  // ---- Durchlaufzeit ----
  // Erst hier, weil erst hier feststeht, wann das Bild qBlank verlaesst.
  // Gemessen wird die Strecke, fuer die dieses Programm geradesteht: von der
  // Ankunft in der Senke bis zu dem Augenblick, in dem Present zurueckkehrt und
  // das Bild dem Compositor gehoert. Alles dazwischen zaehlt mit -- das
  // Hochladen, Deinterlacing, Filter, Skalierung, das Zeichnen, die
  // Present-Warteschlange und bei eingeschaltetem VSync das Warten auf den
  // Bildwechsel.
  //
  // Was davor liegt (Halbbildaufnahme, Karte, Treiber, Transport) und was
  // danach kommt (Compositor, Kabel, die Elektronik des Schirms), ist von hier
  // aus nicht messbar und deshalb auch nicht enthalten. Die Zahl ist der
  // Beitrag von qBlank, nicht das Alter des Lichts.
  //
  // Vorher stand hier die Ankunftszeit gegen den Zeichenbeginn, und weil die
  // Schleife auf das Bildereignis wartet, war das fast immer dieselbe Zehntel
  // Millisekunde -- eine Zahl, die nur sagte, dass der Renderthread wach
  // geworden ist.
  if (captureState_ == CaptureState::Running && displayedArrivalQpc_ != 0) {
    const int64_t leaving = ClockTicks();
    const double ageMs = TicksToSeconds(leaving - displayedArrivalQpc_) * 1000.0;
    if (ageMs >= 0.0) frameAgeMeter_.Sample(ageMs, TicksToSeconds(leaving));
  }

  // Erst nach der Durchlaufzeit: was hier gerechnet wird, soll weder das Bild
  // aufhalten noch in der Zahl stehen, die sagt, wie lange es aufgehalten wurde.
  renderer_.AnalyzeAfterPresent();

  // ---- present rate ----
  ++presentCount_;
  if (fpsWindowQpc_ == 0) fpsWindowQpc_ = now;
  const double elapsed = TicksToSeconds(now - fpsWindowQpc_);
  if (elapsed >= 1.0) {
    presentFps_ = presentCount_ / elapsed;
    presentCount_ = 0;
    fpsWindowQpc_ = now;


    // With logging on, write a line every few seconds. This is what makes a
    // "it stutters" report actionable without having to reproduce it here.
    if (config_.app.logToFile && captureState_ == CaptureState::Running) {
      if (++statsLogCounter_ >= 5) {
        statsLogCounter_ = 0;
        const SinkStats sinkStats = sink ? sink->stats() : SinkStats{};
        const AudioStats audioStats = audio_.stats();
        // Mittel und Spitze ueber die ganzen fuenf Sekunden statt eines
        // Augenblickswerts. Ein Ruckler ist ein einzelnes Bild -- die Chance,
        // ihn mit einer Stichprobe alle fuenf Sekunden zu erwischen, ist etwa
        // eins zu dreihundert, und genau danach wird in diesen Zeilen gesucht.
        double ageHigh = 0.0;
        frameAgeMeter_.TakeRange(nullptr, &ageHigh);
        double bufLow = 0.0, bufHigh = 0.0;
        audioBufferMeter_.TakeRange(&bufLow, &bufHigh);
        CAP_LOG("Status: source %.2f fps, output %.1f fps, %llu shown, %llu dropped, frame age "
                "%.1f ms (peak %.1f) | fields %llu/%llu | audio %.1f/%.0f ms (%.1f-%.1f), %llu "
                "underruns, %llu overruns",
                sinkStats.sourceFps, presentFps_, (unsigned long long)sinkStats.displayed,
                (unsigned long long)sinkStats.dropped, frameAgeMeter_.average, ageHigh,
                (unsigned long long)fieldsShown_[0], (unsigned long long)fieldsShown_[1],
                audioBufferMeter_.average, audioStats.targetMs, bufLow, bufHigh,
                (unsigned long long)audioStats.underruns,
                (unsigned long long)audioStats.overruns);
      }
    }
  }
}

void App::FeedFrameConsumers() {
  const bool wantRecorder = recorder_.recording();
  // Only while something is actually watching. An idle camera costs a flag.
  const bool wantCamera = virtualCamera_.running() && virtualCamera_.consumed();

  renderer_.SetHdrWideWanted(wantRecorder && config_.app.recordHdr,
                             wantCamera && virtualCamera_.wantsWide());
  renderer_.SetReadbackEnabled(wantRecorder || wantCamera);
  if (!wantRecorder && !wantCamera) return;

  // Either of the two can be on the wide path while the other is not, so both
  // rings can be live at once. They are read in one place so a frame is never
  // fetched twice and never half handed out.
  const bool wide = renderer_.hdrWideActive();
  const bool recordWide = wantRecorder && wide && config_.app.recordHdr;
  const bool cameraWide = wantCamera && wide && virtualCamera_.wantsWide();

  if (recordWide || cameraWide) {
    VideoRenderer::ReadbackFrame w;
    if (renderer_.FetchHdrReadback(&w)) {
      if (recordWide) recorder_.PushVideo(w.data, w.stride, w.width, w.height);
      if (cameraWide) virtualCamera_.PushFrameWide(w.data, w.stride, w.width, w.height);
      renderer_.ReleaseHdrReadback();
    }
  }

  if (!(wantCamera && !cameraWide) && !(wantRecorder && !recordWide)) return;

  VideoRenderer::ReadbackFrame frame;
  if (!renderer_.FetchReadback(&frame)) return;
  if (wantRecorder && !recordWide) {
    recorder_.PushVideo(frame.data, frame.stride, frame.width, frame.height);
  }
  if (wantCamera && !cameraWide) {
    virtualCamera_.PushFrame(frame.data, frame.stride, frame.width, frame.height);
  }
  renderer_.ReleaseReadback();
}

void App::UpdateVirtualCamera() {
  const int request = settings_.takeVirtualCameraRequest();
  if (request != 0) {
    // Installing while it runs would pull the source out from under a reader,
    // so it goes off first either way.
    const bool wasOn = config_.app.virtualCamera;
    if (virtualCamera_.running()) virtualCamera_.Stop();

    std::string error;
    const bool ok = request == 1 ? CameraSink::InstallSystemWide(&error)
                                 : CameraSink::RemoveSystemWide(&error);
    if (ok) {
      Toast(request == 1 ? T("Kamera installiert.", "Camera installed.")
                         : T("Kamera deinstalliert.", "Camera uninstalled."));
      if (request == 2) config_.app.virtualCamera = false;
    } else {
      Toast(error);
      config_.app.virtualCamera = wasOn;
    }
  }

  virtualCamera_.SetWideOffered(config_.app.cameraHdr);

  const bool want = config_.app.virtualCamera;
  if (want && !virtualCamera_.running() && !virtualCamera_.starting()) {
    virtualCamera_.StartAsync();
  } else if (!want && (virtualCamera_.running() || virtualCamera_.starting())) {
    virtualCamera_.Stop();
  }

  std::string startError;
  if (virtualCamera_.takeError(&startError)) {
    // Turning the switch back off rather than leaving it on and doing nothing,
    // so the tab does not claim a camera that is not there.
    config_.app.virtualCamera = false;
    Toast(startError);
  }

  // What the camera would hand out right now, written whether or not anybody is
  // listening. A consumer reads all of this the moment it connects -- before it
  // has asked for a frame -- so it has to be there beforehand rather than be
  // discovered from the first picture.
  const VideoFormatInfo& format = renderer_.sourceFormat();
  const bool wide = config_.app.cameraHdr &&
                    renderer_.hdrTransfer() != VideoRenderer::Transfer::Sdr;
  virtualCamera_.SetSourceShape(renderer_.outputWidth(), renderer_.outputHeight(), format.fps,
                                wide);

  virtualCamera_.consumers(&virtualCameraConsumers_);
  settings_.SetVirtualCameraState(virtualCamera_.running(), virtualCameraConsumers_);
}

void App::UpdateHdr() {
  const VideoFormatInfo& src = renderer_.sourceFormat();

  VideoRenderer::Transfer transfer = VideoRenderer::Transfer::Sdr;
  bool wideGamut = false;
  switch (config_.app.hdrInput) {
    case HdrInput::Pq:
      transfer = VideoRenderer::Transfer::Pq;
      wideGamut = true;
      break;
    case HdrInput::Hlg:
      transfer = VideoRenderer::Transfer::Hlg;
      wideGamut = true;
      break;
    case HdrInput::Sdr:
      break;
    case HdrInput::Auto:
    default:
      if (src.color.transfer == ColorInfo::Transfer::PQ) transfer = VideoRenderer::Transfer::Pq;
      if (src.color.transfer == ColorInfo::Transfer::HLG) transfer = VideoRenderer::Transfer::Hlg;
      // BT.2020 primaries; the matrix says the same thing a second way.
      wideGamut = src.color.primaries == ColorInfo::Primaries::BT2020 ||
                  src.color.matrix == ColorInfo::Matrix::BT2020_10 ||
                  src.color.matrix == ColorInfo::Matrix::BT2020_12;
      break;
  }
  renderer_.SetHdrInput(transfer, wideGamut);

  // The screen can change without anything else doing so -- dragging the window
  // to another monitor, or turning HDR on in Windows while this runs. Asking
  // costs a little, so not every frame.
  if (++hdrDisplayPoll_ >= 120) {
    hdrDisplayPoll_ = 0;
    display_.RefreshDisplayCapability();
  }

  const Display::DisplayCapability display = display_.displayCapability();
  bool want = false;
  switch (config_.app.hdrOutput) {
    case HdrOutput::Always:
      want = display.hdr;
      break;
    case HdrOutput::Auto:
      // Only when there is something to gain. An ordinary picture on an HDR
      // screen goes through one more conversion for no benefit.
      want = display.hdr && transfer != VideoRenderer::Transfer::Sdr;
      break;
    case HdrOutput::Off:
    default:
      break;
  }

  if (want != display_.hdrOutput()) {
    std::string error;
    if (!display_.SetHdrOutput(want, &error)) {
      // Said once and then left alone, rather than every frame from here on.
      if (!error.empty() && want) {
        config_.app.hdrOutput = HdrOutput::Off;
        Toast(error);
      }
    }
  }

  renderer_.SetHdrOutput(display_.hdrOutput(), config_.app.paperWhiteNits,
                         config_.app.sourcePeakNits,
                         display_.hdrOutput() ? display.peakNits : 100.0f);

  settings_.SetHdrState(display.hdr, display_.hdrOutput(), display.peakNits, (int)transfer);
  settings_.SetCarrierPeriod(renderer_.effectiveCarrierPeriod());
}

void App::WriteScreenshot(bool includeUi, bool toClipboard) {
  RecordSettings& rec = config_.record;

  std::vector<uint8_t> pixels;
  std::vector<uint16_t> halfPixels;
  int width = 0, height = 0, halfStride = 0;
  bool wide = false;
  std::string note;

  if (includeUi) {
    // The whole window as it stands, which is what "with the interface" has to
    // mean: the picture scaled into the window, the bar, the overlays. Only
    // possible in eight bit -- an HDR back buffer is scRGB float, and the
    // conversion out of it is not something to guess at. There, the picture
    // itself is saved instead and the message says so.
    if (!display_.GrabBackBuffer(&pixels, &width, &height)) {
      includeUi = false;
      note = T(" (ohne Oberfläche, HDR-Ausgabe)", " (without interface, HDR output)");
    }
  }

  if (!includeUi) {
    // Keeping the range only means anything when there is a range to keep, so
    // the setting and the source both have to say so. Otherwise this is an
    // ordinary screenshot and takes the ordinary path. The clipboard is always
    // the ordinary path: there is no way to hand a wide range over it.
    wide = !toClipboard && config_.app.screenshotHdr &&
           renderer_.hdrTransfer() != VideoRenderer::Transfer::Sdr;
    if (wide) {
      if (!renderer_.GrabStillHalf(&halfPixels, &width, &height, &halfStride)) {
        Toast(T("Kein Bild zum Speichern.", "No picture to save."));
        return;
      }
    } else if (!renderer_.GrabStill(&pixels, &width, &height)) {
      Toast(T("Kein Bild zum Speichern.", "No picture to save."));
      return;
    }
  }

  if (toClipboard) {
    std::string clipError;
    if (!CopyScreenshotToClipboard(&window_, pixels.data(), width, height, &clipError)) {
      Toast(T("Kopieren fehlgeschlagen: ", "Copy failed: ") + clipError);
      return;
    }
    Toast(T("In der Zwischenablage", "On the clipboard") +
          Format(" (%dx%d)", width, height) + note);
    CAP_LOG("Screenshot copied to the clipboard (%dx%d)", width, height);
    return;
  }

  const std::filesystem::path folder =
      ResolveOutputFolder(&rec.screenshotFolder, DefaultScreenshotFolder());
  const std::filesystem::path path =
      folder.empty() ? std::filesystem::path()
      : wide          ? MakeHdrScreenshotPath(folder, config_.app.hdrShotFormat)
                      : MakeScreenshotPath(folder, rec.screenshotFormat);
  if (path.empty()) {
    Toast(T("Zielordner nicht verfügbar.", "Folder not available."));
    CAP_ERR("Screenshot: folder not available: %s", PathToUtf8(folder).c_str());
    return;
  }

  std::string error;
  const bool ok =
      !wide ? SaveScreenshot(path, pixels.data(), width, height, rec.screenshotFormat,
                             rec.jpegQuality, &error)
      : config_.app.hdrShotFormat == HdrShotFormat::Avif
          ? SaveScreenshotAvif(path, Utf8ToPath(recording_.ffmpeg().path), halfPixels.data(),
                               width, height, halfStride, config_.app.paperWhiteNits, &error)
          : SaveScreenshotHdr(path, halfPixels.data(), width, height, halfStride,
                              config_.app.paperWhiteNits, &error);
  if (!ok) {
    Toast(T("Screenshot fehlgeschlagen: ", "Screenshot failed: ") + error);
    return;
  }

  // The file name, not the whole path: the path is long, and the point of the
  // message is "it worked and it is called this".
  Toast(T("Screenshot: ", "Screenshot: ") + PathToUtf8(path.filename()) + note, path);
  CAP_LOG("Screenshot saved: %s (%dx%d)", PathToUtf8(path).c_str(), width, height);
}

}  // namespace cap
