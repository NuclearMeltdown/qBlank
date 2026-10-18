#pragma once

// Saves screenshots, off the thread that draws.
//
// The picture is copied out where it is drawn, that has to be; the rest does
// not. A PNG of a large picture takes longer to encode than a frame lasts, AVIF
// goes through a whole ffmpeg run, the clipboard can be busy, and even finding
// the folder can wait for a drive to spin up. SaveShot does all of that, on the
// thread the app keeps for screenshots alone: one after the other, which is
// also what keeps two in the same second from picking the same name.

#include <filesystem>
#include <memory>
#include <string>

#include "config.h"
#include "render/still_copy.h"

namespace cap {

class Window;

struct ScreenshotJob {
  // Eight bits for an ordinary shot, half floats for one that keeps the range.
  std::shared_ptr<StillPixels> pixels;
  // Onto the clipboard instead of into a file, for `clipboardOwner`.
  bool toClipboard = false;
  const Window* clipboardOwner = nullptr;
  // As configured: empty for the default.
  std::string folder;
  ScreenshotFormat format = ScreenshotFormat::Png;
  int jpegQuality = 92;
  HdrShotFormat hdrFormat = HdrShotFormat::Jxr;
  std::filesystem::path ffmpeg;
  float paperWhiteNits = 0.0f;
  // Passed through to the result, for the message.
  std::string note;
};

struct ScreenshotResult {
  bool ok = false;
  bool clipboard = false;
  // No folder could be made; `path` is the one that was tried.
  bool noFolder = false;
  // `askedFolder`, the job's, could not be used and the default was.
  bool fellBack = false;
  std::string askedFolder;
  std::filesystem::path path;
  std::string error;
  std::string note;
  int width = 0;
  int height = 0;
};

// Reads the pixels out of their copy and saves them. Any thread but the one
// that draws.
ScreenshotResult SaveShot(const ScreenshotJob& job);

}  // namespace cap
