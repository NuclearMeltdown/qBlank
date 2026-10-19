// The part of screenshot.h that is the same everywhere: AVIF through ffmpeg,
// and where the files go. The encoders Windows ships are in screenshot_win32.cpp.

#include "record/screenshot.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "app_files.h"
#include "child_process.h"
#include "files.h"
#include "i18n.h"

namespace cap {

namespace {

// One IEEE half float, widened. Exact: every half has a float that is equal
// to it.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1fu;
  const uint32_t mantissa = h & 0x3ffu;
  if (exponent == 0) {
    // Zero and the subnormals: the mantissa in units of 2^-24.
    const float f = (float)mantissa * (1.0f / 16777216.0f);
    return sign ? -f : f;
  }
  const uint32_t bits = exponent == 31 ? sign | 0x7f800000u | (mantissa << 13)
                                       : sign | ((exponent + 112) << 23) | (mantissa << 13);
  float f;
  std::memcpy(&f, &bits, sizeof f);
  return f;
}

// Linear light with 1.0 at diffuse white, onto the PQ curve in BT.2020 and into
// the ten bit packing DXGI and ffmpeg agree on. The same arithmetic the shader
// does for the recording path, in software because a still is one frame.
void HalfToPq10(const uint16_t* halfRgba, int width, int height, int stride,
                float paperWhiteNits, std::vector<uint32_t>* out) {
  out->resize((size_t)width * height);
  for (int y = 0; y < height; ++y) {
    const uint16_t* src = halfRgba + (size_t)y * ((size_t)stride / sizeof(uint16_t));
    uint32_t* dst = out->data() + (size_t)y * width;
    for (int x = 0; x < width; ++x) {
      float rgb[3];
      for (int c = 0; c < 3; ++c) {
        rgb[c] = std::max(HalfToFloat(src[(size_t)x * 4 + c]), 0.0f) * paperWhiteNits;
      }
      const float r = 0.627404f * rgb[0] + 0.329283f * rgb[1] + 0.043313f * rgb[2];
      const float g = 0.069097f * rgb[0] + 0.919540f * rgb[1] + 0.011362f * rgb[2];
      const float b = 0.016391f * rgb[0] + 0.088013f * rgb[1] + 0.895595f * rgb[2];

      auto pq = [](float nits) {
        const float m1 = 0.1593017578125f, m2 = 78.84375f;
        const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        const float yv = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
        const float p = std::pow(yv, m1);
        return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
      };
      const uint32_t rc = (uint32_t)std::clamp((int)(pq(r) * 1023.0f + 0.5f), 0, 1023);
      const uint32_t gc = (uint32_t)std::clamp((int)(pq(g) * 1023.0f + 0.5f), 0, 1023);
      const uint32_t bc = (uint32_t)std::clamp((int)(pq(b) * 1023.0f + 0.5f), 0, 1023);
      // R in the low bits, as DXGI packs R10G10B10A2 -- ffmpeg calls it x2bgr10le.
      dst[x] = rc | (gc << 10) | (bc << 20) | (3u << 30);
    }
  }
}

}  // namespace

bool SaveScreenshotAvif(const std::filesystem::path& path, const std::filesystem::path& ffmpegPath,
                        const uint16_t* halfRgba, int width, int height, int stride,
                        float paperWhiteNits, std::string* error) {
  auto fail = [&](const Said& said) {
    CAP_ERR("AVIF screenshot: %s", said.logged.c_str());
    if (error) *error = said.shown;
    return false;
  };
  if (!halfRgba || width <= 0 || height <= 0) return fail(CAP_SAID(T("Kein Bild.", "No picture.")));
  if (ffmpegPath.empty()) {
    return fail(CAP_SAID(T("AVIF braucht ffmpeg. Unter Aufnahme herunterladen, oder JPEG XR wählen.",
                           "AVIF needs ffmpeg. Download it under Recording, or choose JPEG XR.")));
  }

  std::vector<uint32_t> packed;
  HalfToPq10(halfRgba, width, height, stride, paperWhiteNits, &packed);

  // Through a file rather than a pipe. One still is not worth the plumbing, and
  // a temporary file cannot deadlock against a process that stops reading.
  const std::filesystem::path raw = MakeTempFile("cvs");
  if (raw.empty()) {
    return fail(CAP_SAID(T("Kein Platz für die Zwischendatei.", "Nowhere to put the working file.")));
  }
  if (!WriteWholeFile(raw, packed.data(), packed.size() * sizeof(uint32_t))) {
    RemoveFile(raw);
    return fail(CAP_SAID(T("Zwischendatei ließ sich nicht schreiben.",
                           "The working file could not be written.")));
  }

  ProcessSpec spec;
  spec.program = PathToUtf8(ffmpegPath);
  spec.Add("-hide_banner");
  spec.Add("-loglevel", "error");
  spec.Add("-y");
  spec.Add("-f", "rawvideo");
  spec.Add("-pix_fmt", "x2bgr10le");
  spec.Add("-s", std::to_string(width) + "x" + std::to_string(height));
  spec.Add("-i", PathToUtf8(raw));
  spec.Add("-frames:v", "1");
  spec.Add("-c:v", "libaom-av1");
  spec.Add("-still-picture", "1");
  spec.Add("-crf", "18");
  // Matrix and description in the filter as well, as in the recorder: an older
  // ffmpeg would convert with BT.601, and ffmpeg 9 drops the primaries and the
  // transfer below unless the frames carry them.
  spec.Add("-vf", "scale=out_color_matrix=bt2020:out_range=tv,setparams=color_primaries=bt2020:"
                  "color_trc=smpte2084:colorspace=bt2020nc:range=tv");
  spec.Add("-pix_fmt", "yuv420p10le");
  // Without these it is a dark picture rather than an HDR one; nothing else in
  // the file says the samples are on the PQ curve.
  spec.Add("-color_primaries", "bt2020");
  spec.Add("-color_trc", "smpte2084");
  spec.Add("-colorspace", "bt2020nc");
  spec.Add("-f", "avif");
  spec.Add(PathToUtf8(path));

  int code = 1;
  if (!RunAndWait(spec, &code, 60000)) code = 1;
  RemoveFile(raw);

  if (code != 0) {
    return fail(CAP_SAID(T("ffmpeg konnte das AVIF nicht schreiben.",
                           "ffmpeg could not write the AVIF.")));
  }
  return true;
}

std::filesystem::path DefaultScreenshotFolder(const std::string& name) {
  std::filesystem::path folder = UserPicturesFolder();
  if (folder.empty()) folder = ExeFolder();
  return folder / Utf8ToPath(name);
}

std::filesystem::path MakeHdrScreenshotPath(const std::filesystem::path& folder,
                                            HdrShotFormat format) {
  std::filesystem::path base = MakeScreenshotPath(folder, ScreenshotFormat::Png);
  if (base.empty()) return base;
  base.replace_extension(format == HdrShotFormat::Avif ? ".avif" : ".jxr");
  return base;
}

std::filesystem::path MakeScreenshotPath(const std::filesystem::path& folder,
                                         ScreenshotFormat format) {
  if (!EnsureFolder(folder)) return {};

  const std::string ext = format == ScreenshotFormat::Jpeg ? ".jpg" : ".png";
  const LocalTime now = NowLocal();
  const std::string stamp =
      Format("%s_%04d-%02d-%02d_%02d-%02d-%02d", AppNameUtf8().c_str(), now.year, now.month,
             now.day, now.hour, now.minute, now.second);

  // Someone holding the key down produces several shots inside one second, and
  // silently overwriting the earlier ones would be the wrong answer.
  std::filesystem::path candidate = folder / Utf8ToPath(stamp + ext);
  for (int n = 2; IsFile(candidate) && n < 1000; ++n) {
    candidate = folder / Utf8ToPath(stamp + "_" + std::to_string(n) + ext);
  }
  return candidate;
}

}  // namespace cap
