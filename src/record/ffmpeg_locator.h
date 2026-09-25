#pragma once

// Finds ffmpeg.exe and works out which encoders actually work on this machine.
//
// Parsing "ffmpeg -encoders" is not enough: it lists what the build was
// compiled with, not what the hardware can do. A build with h264_nvenc on a
// machine with an AMD card lists it and then fails at record time. So each
// candidate gets a one frame test encode to /dev/null, which takes a moment and
// tells the truth.

#include <filesystem>
#include <string>
#include <vector>

#include "common.h"
#include "config.h"

namespace cap {

struct EncoderInfo {
  RecordEncoder id = RecordEncoder::Auto;
  std::string ffmpegName;  // e.g. "h264_nvenc"
  std::string label;       // what the settings show
  bool hardware = false;
  bool tested = false;     // the test encode has been run for this entry
  bool available = false;  // survived the test encode
  std::string error;       // why not, when it did not
};

struct FfmpegInfo {
  bool found = false;
  std::string path;     // full path to ffmpeg.exe
  std::string version;  // first line of "ffmpeg -version"
  // The release number out of that line, "9.0.1". Empty for a git build, whose
  // line carries a date or a commit count instead and so has nothing a release
  // could be compared against.
  std::string number;
  bool tested = false;  // encoder probe has run
  std::vector<EncoderInfo> encoders;

  const EncoderInfo* Find(RecordEncoder id) const;
  // What the automatic modes resolve to: the first entry of a fixed preference
  // order that passed the test.
  //
  //   compatibility  H.264, then H.265, then AV1 -- hardware ahead of CPU
  //   efficiency     AV1, then H.265, then H.264 -- hardware ahead of CPU
  //
  // There is no scoring. Which order applies is the user's choice, because the
  // two goals genuinely conflict: AV1 is the better codec and the one an older
  // television cannot play.
  const EncoderInfo* BestAvailable(bool preferEfficiency = false) const;

  // Resolves either automatic mode, or looks up a specific encoder.
  const EncoderInfo* Resolve(RecordEncoder wanted) const;
  bool AnyAvailable() const;
};

// Looks in this order: the configured path, an "ffmpeg" folder next to
// qBlank.exe, next to qBlank.exe itself, then PATH. Only fills in path and
// version -- the encoder probe is separate because it takes seconds.
FfmpegInfo LocateFfmpeg(const std::string& configuredPath);

// "ffmpeg version 9.0.1-essentials_build-..." -> "9.0.1". Also takes the
// "n7.1.1" some builds write. Empty unless it is a release number.
std::string FfmpegReleaseNumber(const std::string& versionLine);

// Numerically, part by part: "9.0.10" is newer than "9.0.9", "7.1" equals
// "7.1.0". False when either side is empty.
bool IsNewerFfmpeg(const std::string& remote, const std::string& installed);
// The first number differs. That is where options get removed or renamed, so
// the recording's command line is worth one test afterwards.
bool IsMajorFfmpegStep(const std::string& remote, const std::string& installed);

// Runs a one frame encode per candidate and marks what worked. Slow (several
// seconds for the whole list), so this is called on demand, not at startup.
void ProbeEncoders(FfmpegInfo* info);

// Tests only as much as is needed to answer "can I record right now": the
// requested encoder, or for Auto the preference order until one works. Results
// are remembered, so a second call costs nothing. Returns null when nothing
// usable was found.
const EncoderInfo* EnsureUsableEncoder(FfmpegInfo* info, RecordEncoder wanted);

// The full candidate list with labels, regardless of availability.
std::vector<EncoderInfo> KnownEncoders();

// Fills in the results of an earlier test instead of running one. `available`
// holds RecordEncoder values. Everything not in it counts as tested and
// unavailable, which is the point: the list then shows what this machine can
// actually do.
void ApplyCachedProbe(FfmpegInfo* info, const std::vector<int>& available);

// Default recording folder: Videos\<program name>. The name can be given, which
// is what a build does after taking over the settings of an earlier name: the
// recordings stay in the folder they have always been in rather than quietly
// moving to a folder named after the new one.
std::filesystem::path DefaultRecordFolder(const std::string& name = AppNameUtf8());

}  // namespace cap
