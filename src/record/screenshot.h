#pragma once

// Writes a single frame to disk, or onto the clipboard.
//
// PNG, JPEG and JPEG XR go through what the system ships -- WIC on Windows,
// in screenshot_win32.cpp -- rather than through ffmpeg. A screenshot is the
// one output that should work on a fresh machine where nobody has pressed the
// download button yet. Only AVIF needs ffmpeg.

#include <cstdint>
#include <filesystem>
#include <string>

#include "common.h"
#include "config.h"

namespace cap {

class Window;

// Saves tightly packed RGBA pixels, R first, as the stills hand them over.
// Returns false and fills `error` on failure.
bool SaveScreenshot(const std::filesystem::path& path, const uint8_t* pixels, int width, int height,
                    ScreenshotFormat format, int jpegQuality, std::string* error);

// A screenshot that keeps the range, as JPEG XR holding scRGB half floats --
// which is what Windows itself writes for an HDR screenshot, and therefore what
// the Photos app opens as one. `halfRgba` is four half floats per pixel of
// linear light with 1.0 meaning diffuse white; `paperWhiteNits` is what that
// white should be, and scRGB fixes its own 1.0 at eighty nits.
bool SaveScreenshotHdr(const std::filesystem::path& path, const uint16_t* halfRgba, int width,
                       int height, int stride, float paperWhiteNits, std::string* error);

// The same picture as AVIF, by way of ffmpeg. Ten bit PQ in BT.2020, with the
// colour description that makes it an HDR image rather than a dark one. Needs
// ffmpeg present -- unlike everything else about screenshots.
bool SaveScreenshotAvif(const std::filesystem::path& path, const std::filesystem::path& ffmpegPath,
                        const uint16_t* halfRgba, int width, int height, int stride,
                        float paperWhiteNits, std::string* error);

// The same picture, but onto the clipboard instead of into a file: paste it
// into a chat, a forum post or an image editor without a detour over disk.
//
// It goes on as CF_DIB, twenty-four bit and bottom-up, which is the one shape
// every program that takes a picture from the clipboard understands. A
// thirty-two bit DIB would carry an alpha channel that half of them read as
// transparency and the other half ignore, and video has no alpha to carry.
// There is no HDR form of this: the clipboard has no way to say what the
// numbers on it mean, so the tone mapped picture is what gets copied.
bool CopyScreenshotToClipboard(const Window* owner, const uint8_t* pixels, int width, int height,
                               std::string* error);

// Timestamped name in `folder`, with a counter when the same second is hit
// twice. Creates the folder. Returns empty when the folder cannot be made.
std::filesystem::path MakeScreenshotPath(const std::filesystem::path& folder,
                                         ScreenshotFormat format);
// The same, for the two formats an HDR screenshot can take.
std::filesystem::path MakeHdrScreenshotPath(const std::filesystem::path& folder,
                                            HdrShotFormat format);

// Pictures\<program name>; see DefaultRecordFolder on why the name is an
// argument.
std::filesystem::path DefaultScreenshotFolder(const std::string& name = AppNameUtf8());

}  // namespace cap
