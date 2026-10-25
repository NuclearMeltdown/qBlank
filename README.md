<p align="center">
  <img src="docs/icon.png" width="112" alt="">
</p>

<h1 align="center">qBlank</h1>

<p align="center">
  A low-latency viewer and recorder for capture cards, screens and windows on Windows.
</p>

![The viewer in fullscreen showing a console at 1080p60, with the statistics overlay reading a frame age of 1.2 ms](docs/viewer.jpg)

A lightweight program for playing any console through a capture card: one
file, any DirectShow device, and as little delay as the hardware allows.
Measured on a StarTech PEXHDCAP60L: **1080p60 sustained, around 1 ms from a
frame arriving to the present that hands it to the compositor.**

No card needed for the rest: qBlank also records your screen or a single
window, PC games included, with the same recording, replay and screenshots.

Recording (lossless too), a replay buffer, screenshots and a virtual camera
are included; scenes, overlays, compositing and streaming are not. For
those, use OBS.

> **The [manual](https://qblank.com/manual.html) explains how
> to use it** (English and German). **The [wiki](../../wiki) is the documentation** —
> one page per feature, what the code does and what was measured. This page is the list.

## Latency

- No queue in the capture path: triple buffer, late frames dropped.
- No graph clock, so nothing waits for a presentation time.
- Flip-model swap chain, maximum frame latency of one, tearing permitted, VSync off.
- Direct3D 11 by default; Vulkan as an option under *Settings → General*, with the same passes and the same rules.
- Format conversion on the GPU: YUY2, UYVY, YVYU, NV12, planar 4:2:0, RGB, P010.
- Nothing on the render thread waits on a disk, a driver or the shell: each area has a thread of its own.

[Latency](../../wiki/Latency)

## Features

**Source** · [wiki](../../wiki/Source-and-signal)
- Any DirectShow video device.
- Resolution, rate, pixel format and colour space picked independently — undocumented but working combinations can be forced.
- Rate list carries *highest available* and *the signal's rate*.
- A resolution that does not fit the video standard is corrected, or asked about on the picture.
- Analogue: video standard, input cable, and the driver's own property pages while the picture runs.

**Screen and window** · [wiki](../../wiki/Screen-and-window)
- *Source → Screen*: a monitor through Desktop Duplication, copied on the GPU.
- *Source → Window*: one program's window through Windows.Graphics.Capture — covered is fine, minimised is not; found again when it reopens.
- Mouse pointer optional; a preview, or a status panel that draws less.
- qBlank hides itself from the screen capture — optional, so Remote Play can still see it.
- A notice when a game in exclusive fullscreen keeps the picture away; borderless windowed works.
- Sound through the recording source *System sound without qBlank*.

**Automatic video standard** · [wiki](../../wiki/Automatic-video-standard)
- Rechecked for as long as the program runs, not chosen once at startup.
- A colour round measures the picture — PAL B, PAL N and SECAM all fit 625 lines.
- A settled standard's colour keeps being watched; one that turns out wrong is searched again.
- 1.7–2.4 s from wrong to confirmed. **F7** searches by hand.

**Picture** · [wiki](../../wiki/Scaling-and-sharpening)
- Nearest, bilinear, Catmull-Rom, Lanczos3, sharp-bilinear.
- Contrast adaptive sharpening; brightness, contrast, saturation, hue.
- Aspect override, integer scaling, square pixels, quarter turns, line doubling for 240p and 288p.
- **Native pixel grid**: one output pixel per console pixel, where the card samples a line 720 times and a SNES drew 256. The grid is measured from the picture; on analogue sources each pixel is rebuilt from the card's blurred samples and cleaned up. Without a width set, qBlank notices pixel art and suggests one.
- **Freeze** (**F11**) holds the source, so a slider can be judged on a still picture.
- **A/B compare** (**F12**) splits the picture, every filter off on one side, deinterlacing on both. Vertical or horizontal, chosen in the right-click menu.
- **All filters off** (**Shift+F12**) shows the signal as it arrives, with deinterlacing, crop, aspect and range kept.

![Left: the Super Mario World logo from a PAL SNES over composite, snapped to the native pixel grid in qBlank 5.6.0, outlines grey and colours bleeding. Right: the same frame in 5.7.0, black outlines and one colour per letter](docs/pixel-grid-logo.png)

![Left: the copyright line in 5.6.0, blurred and in many shades of blue. Right: 5.7.0, one cyan with a black outline](docs/pixel-grid-text.png)

**Crop and colour range** · [wiki](../../wiki/Cropping-and-geometry)
- Dragged on the picture or found by **Detect** (**F8**).
- Measured across two seconds, so a fade to black is not read as shrinking.
- Counted in source pixels; dropped or kept per picture size when the source changes.
- Range and matrix measured from the image. **F6** measures again.

![The crop editor over the running picture: handles on all four edges, and a toolbar carrying the four values, the resulting size, Apply and Cancel](docs/crop-configure.jpg)

![The Fields and Crop sections of the settings: Measured: interlaced above the deinterlacer, the four crop values with Configure, Detect and Reset, and the picture sizes the crop is already stored for](docs/settings-fields-crop.png)

**Deinterlacing** · [wiki](../../wiki/Deinterlacing) — interlacing is measured, not believed.

| Mode | Vertical movement | |
|---|---|---|
| Off (weave) | none | combing on anything that moves |
| Bob | **1.0 line** | full rate, no latency, no interpolation |
| Bob interpolated | 0.56 | alternates sharp and interpolated lines |
| Motion adaptive | **0.005** | weaves what is still, interpolates what is not |
| Edge directed | 0.69 | follows edges; meant for pixel art |
| YADIF | **0.002** | best quality; keeps one frame in memory |

![Left: a kart driving past a 480i barrier, woven, the chevrons torn into interlacing combs. Right: the same moment through YADIF, clean](docs/deinterlace-before-after.png)

**Composite filter** · [wiki](../../wiki/The-composite-filter)
- **Extended history** looks twelve frames back and takes the dot crawl out of every frame, also where something moves or fades: around text, in transparencies. Where the crawl repeats every 2 or 4 frames (PAL, PAL 60, PAL M, PAL N, NTSC). GPU time, no frame of delay.
- **Average what stands still** clears dot crawl where the picture stands still, at no cost in sharpness. It averages over the crawl cycle measured for the source: 4 frames for PAL, 2 or 3 elsewhere.
- **Synchronous demodulator** takes over where it moves; **follow the movement** averages along the path, sideways and up and down.
- Weighted sideways average against colour shimmer; **restore bandwidth** puts back the rolled-off top of the band.
- Derived in the shader from the subcarrier period: right for PAL, PAL 60, NTSC, NTSC 4.43, PAL M and PAL N at any width. SECAM approximated.

![Left: a GameCube over composite with the filter off, dot crawl beading along every letter edge and across the colour bars. Right: the same frame with what stands still averaged, clean](docs/composite-before-after.png)

![A title screen under F12, magnified across the divider: the logo and the lettering beaded with dot crawl on the left half, clean on the right](docs/compare-dotcrawl.jpg)

![The Composite filter section of the settings: colour shimmer, the average of what stands still with its conditions, the demodulator at step 9 of 9 reading 81 % gone for 17 % softer, and restore bandwidth](docs/settings-composite.png)

**Cathode ray tube** — off by default, display only.
- Scanline gaps follow the **source's** line grid, not the screen's, and are absent where there is no room.
- **Source lines** says what the console drew, for a dongle handing over 1080 lines from 480.
- Aperture grille or shadow mask; both put back the brightness they take.

![Left: the Super Mario World title screen at 240p, magnified. Right: the same picture with scanlines at 0.35, a dark gap between each pair of picture lines](docs/crt-scanlines.png)

![Three panels magnified to the pixel: mask off, aperture grille with its vertical stripes, and a shadow mask whose triads step sideways every other row](docs/crt-mask.png)

![The Cathode ray tube and Colour sections of the settings: source lines, scanlines and mask above the range and matrix, with the measurement under them — full range, 53.41 % below 16, and the note that an analogue input is not sitting on 16](docs/settings-crt-colour.png)

**High dynamic range** · [wiki](../../wiki/High-dynamic-range)
- P010 and P016 read against PQ (ST 2084) or HLG (BT.2100).
- BT.2390 tone mapping on an ordinary screen, scRGB on an HDR screen.
- Recording, screenshots and the camera tone map by default, or keep the range.
- An HDR desktop or window is captured in HDR.

**Audio** · [wiki](../../wiki/Audio)
- Embedded card audio or any Windows recording device, out through WASAPI.
- Buffer target, optional exclusive mode, A/V offset.
- Clock drift corrected by nudging the playback rate a fraction of a per cent.
- Recording sources on top of the card audio, as many as needed: microphones, output devices, the system sound without qBlank — each with its own gain, mixed, separate or both, never played back.

**Recording** · [wiki](../../wiki/Recording)
- H.264, H.265 or AV1 through NVENC, Quick Sync, AMF, x264, x265 or SVT-AV1, encoded inside qBlank by ffmpeg's libraries.
- AAC at 96 to 320 kbit/s per track; PCM in AVI.
- 4:2:0 or full colour resolution (4:4:4), converted on the GPU; zero-copy into NVENC and AMF.
- Lossless: H.265 lossless (NVENC) in MKV, UT Video or uncompressed RGB in AVI, no 4 GB limit.
- **Unfiltered**: the card's frames as they arrive — no deinterlacing, crop, scaling or filters.
- At source resolution: after crop and deinterlacing, before window scaling.
- Capture audio is the master clock — constant frame rate, 1 ms drift over 15 seconds.
- 60 to 50 Hz mid-recording **cuts the file and continues in a new one**.
- Rate control, preset, tuning, look-ahead, adaptive quantisation and multipass under one set of names.
- Free space shown as remaining recording time; a recording will not start on a full drive and stops itself before one fills up.
- An optional larger memory buffer for slow drives, never more than is free.
- **Resolution**: caps the short side (2160p … 360p); display and screenshots stay full.
- **All encoder chips**: NVENC H.265 and AV1 split each frame over every NVENC on the card — RTX 4080, AV1 1440p p7: 110 → 183 fps.
- Frames the encoder cannot take are left out, not repeated; a notice says so.
- **Pause** pauses and resumes in one file, without a gap; the replay goes on.
- **Ctrl+Shift+F9** sets a marker: a chapter in MKV and MP4, in replay clips too.
- Lossless files go to `raw\`, clips to `clips\`.

**Replay** · [wiki](../../wiki/Recording#replay)
- The last 5 s to 30 min, held in memory or on disk.
- **Shift+F9** saves a clip, **Ctrl+F9** switches it on and off.
- Runs beside a recording on the same encoder, no second one.

**System-wide shortcuts** · [wiki](../../wiki/Shortcuts#system-wide)
- Any action while a game has the keyboard: record, replay, screenshot, fullscreen, volume …
- A set of their own under *Settings → Keys*, none bound out of the box.
- A combination another program holds is shown in red.

**Screenshots** · [wiki](../../wiki/Screenshots)
- At source resolution, taken **before the interface is drawn** — or the whole window with it, if you ask.
- PNG and JPEG through Windows Imaging Component — **no ffmpeg needed**.
- HDR sources can keep their range as JPEG XR or AVIF.
- **Ctrl+F10** puts the shot on the clipboard instead of on disk.

**Virtual camera** · [wiki](../../wiki/Virtual-camera)
- Offered to other programs as **qBlank Virtual Camera**, at the source's own resolution and rate.
- Installing costs one UAC prompt.
- **Leave the reading program's resolution on automatic** — in OBS, *Resolution/FPS Type: Device Default*.

**Profiles**
- One per console: device, input, standard, capture format, picture and audio settings.
- **Ctrl+1** … **Ctrl+9** switch between them.
- A profile can say **which video standard means it** — same cable, two consoles, no keystroke.

**The main window** · [wiki](../../wiki/The-main-window)
- 1×, 2× or 3× the source's lines from the context menu.
- Statistics level — compact, normal or full — from the context menu too.
- **Shift** while resizing keeps the picture's shape.
- Borderless keeps its taskbar button, Alt+Tab and Snap; drag the picture to move it.
- A red dot on the taskbar button while recording.
- An optional tray icon with quick actions you pick; the taskbar button stays.
- Start menu and desktop shortcuts from *Settings → General*, offered on the first start.

**The settings window** · [wiki](../../wiki/The-settings-window)
- Its own window with its own graphics device, so it can go on a second monitor.
- Tabs wrap into rows rather than being cut short, and the layout follows a resize live.
- An embedded panel remains, because a window capture in OBS cannot see a second window.
- Controls that cannot apply to the source are absent rather than disabled, and are not applied.
- **Ctrl+F** finds any setting, through typos and the names other programs use.
- Right click on a slider puts it back to its default.
- An *About* tab with version, licence, links to the website, the manual and GitHub, and the update check.

**Updates** · [wiki](../../wiki/Updates)
- Compares the build against the newest GitHub release.
- Installs by renaming, so a failed update leaves the program as it was.

## Shortcuts

| Key | Action | | Key | Action |
|---|---|---|---|---|
| Enter / Esc | Fullscreen on / off | | F9 | Start / stop recording |
| Ctrl+Enter | Borderless | | Pause | Pause / resume recording |
| F1 | Statistics | | Shift+F9 | Save replay as clip |
| F2 | Settings | | Ctrl+F9 | Replay on / off |
| F3 | Toolbar | | Ctrl+Shift+F9 | Set marker |
| F5 | Restart capture | | F10 | Screenshot |
| Shift+F5 | Reinitialise card | | Ctrl+F10 | Screenshot to clipboard |
| F6 | Measure colour range | | F11 | Freeze |
| F7 | Detect video standard | | F12 | Compare filters |
| F8 | Detect border | | Shift+F12 | All filters off |
| | | | M | Mute |
| | | | `+` `-` / wheel | Volume |
| | | | Ctrl+1 … Ctrl+9 | Switch profile |

Right click opens the menu, a double click on the picture toggles fullscreen.
A click on the notice after a recording or screenshot shows the file in
Explorer. All except Esc, the profile digits and Alt+F4 are reassignable under
*Settings → Keys*. [Shortcuts](../../wiki/Shortcuts)

## ffmpeg

- Needed for **recording**, the **replay**, **remuxing to MP4** and **HDR screenshots in AVIF**, nothing else.
- Preview, filters, deinterlacing, virtual camera and SDR screenshots run without it.
- Not bundled. *Settings → Encoder* downloads the four libraries of a shared build (57 MB) and verifies its published SHA-256.
- Loaded into qBlank itself: no second process, no pipe.
- Encoders are found by test-encoding two frames, not by reading a list.

[ffmpeg](../../wiki/ffmpeg)

## Limitations

- **One process holds a card's capture pin.** If OBS has it, qBlank cannot open it, and the other way round.
- **The virtual camera is invisible to packaged apps.** Its shared memory is in the session namespace, which an app container cannot see: not the Windows Camera app, not Store builds of Teams. OBS, Discord, browsers, vMix and XSplit find it.
- **S-Video and component have not been run.** Every analogue measurement was taken on composite, from a PAL SNES and a GameCube.
- **The HDR display path is untested on HDR hardware.** Tone mapping is verified; scRGB output has never met an HDR monitor, nor has HDR screen capture.
- **SECAM is approximated.** Two alternating subcarriers against qBlank's single figure; the demodulator does not handle it at all.

## Building

Visual Studio 2022 with the Desktop C++ workload, and CMake. Dear ImGui is
vendored in `third_party/`. The Vulkan renderer needs the Vulkan SDK; without
it the build is Direct3D 11 only, with a warning.

```bash
build.bat
```

- `qBlank.exe` lands in the repository root, about 3 MB, static CRT.
- `build.bat keep` retains the build tree, `build.bat debug` builds a debug configuration.
- Settings live in `qBlank.json` beside the executable; nothing goes into the registry.
- Prebuilt executables are attached to each [release](../../releases). [Building](../../wiki/Building)
- Or install through winget: `winget install qBlank` (coming soon — the package is still in review).

## Why DirectShow

Cards with a Media Foundation driver also expose DirectShow; the reverse does not
hold, and older or semi-professional cards are often DirectShow only. On the
development machine DirectShow enumerates five video devices, Media Foundation
three.

## Coming from CapView

- Called CapView up to 3.7. Nothing has to be done by hand: *Settings → Updates* in 3.7 finds the release.
- Settings and profiles carry over; a `CapView.json` beside the program is adopted, not replaced.
- Each release ships a small `CapView.exe` so the 3.7 updater finds an asset under the name it looks for.
- The virtual camera has to be installed again — it is registered under a new name.

## Licence

**GNU General Public License, version 3 or later.** Use it for anything,
including at work and including making money with it; if you pass it on,
modified or not, it goes on under the same terms and with the source. Full text
in [LICENSE](LICENSE).

Up to and including 3.7 the program was MIT, and copies taken while that applied
keep MIT. Dear ImGui is MIT and stays MIT; see
[THIRD-PARTY.md](THIRD-PARTY.md). ffmpeg is not shipped: its libraries are
downloaded from upstream and loaded at runtime, and that build is GPLv3 too.

Written with the help of [Claude](https://claude.ai).
