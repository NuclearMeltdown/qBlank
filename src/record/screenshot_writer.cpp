#include "record/screenshot_writer.h"

#include "common.h"
#include "files.h"
#include "record/screenshot.h"

namespace cap {

// WIC wants COM, which the job thread has set up.
ScreenshotResult SaveShot(const ScreenshotJob& job) {
  StillPixels& pixels = *job.pixels;
  pixels.Read();

  ScreenshotResult result;
  result.note = job.note;
  result.width = pixels.width;
  result.height = pixels.height;

  if (job.toClipboard) {
    result.clipboard = true;
    result.ok = CopyScreenshotToClipboard(job.clipboardOwner, pixels.rgba().data(), pixels.width,
                                          pixels.height, &result.error);
    return result;
  }

  const bool wide = pixels.layout == StillLayout::Half4;
  const FolderChoice choice = ChooseFolder(job.folder, DefaultScreenshotFolder());
  result.fellBack = choice.fellBack;
  result.askedFolder = job.folder;
  if (!choice.folder.empty()) {
    result.path = wide ? MakeHdrScreenshotPath(choice.folder, job.hdrFormat)
                       : MakeScreenshotPath(choice.folder, job.format);
  }
  if (result.path.empty()) {
    result.noFolder = true;
    result.path = choice.folder.empty() ? Utf8ToPath(job.folder) : choice.folder;
    return result;
  }

  // Tightly packed, so a row of half floats is the width's worth.
  const int halfStride = pixels.width * 8;
  if (!wide) {
    result.ok = SaveScreenshot(result.path, pixels.rgba().data(), pixels.width, pixels.height,
                               job.format, job.jpegQuality, &result.error);
  } else if (job.hdrFormat == HdrShotFormat::Avif) {
    result.ok = SaveScreenshotAvif(result.path, job.ffmpeg, pixels.half().data(), pixels.width,
                                   pixels.height, halfStride, job.paperWhiteNits, &result.error);
  } else {
    result.ok = SaveScreenshotHdr(result.path, pixels.half().data(), pixels.width, pixels.height,
                                  halfStride, job.paperWhiteNits, &result.error);
  }
  return result;
}

}  // namespace cap
