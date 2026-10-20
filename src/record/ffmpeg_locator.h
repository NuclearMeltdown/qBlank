#pragma once

// Loads the ffmpeg libraries and works out which encoders actually work on this
// machine.
//
// That a build has an encoder is not enough: it says what the build was
// compiled with, not what the hardware can do. A build with h264_nvenc on a
// machine with an AMD card has it and then fails at record time. So each
// candidate gets a two frame test encode into nothing, which takes a moment and
// tells the truth.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "config.h"

namespace cap {

struct EncoderInfo {
  RecordEncoder id = RecordEncoder::Auto;
  std::string ffmpegName;  // e.g. "h264_nvenc"
  // What the log and the statistics show. The settings ask RecordEncoderName,
  // which follows a change of language.
  std::string label;
  bool hardware = false;
  bool tested = false;     // the test encode has been run for this entry
  bool available = false;  // survived the test encode
  std::string error;       // why not, when it did not
  // 4:4:4, tried after 4:2:0 passed and only where it can work at all
  // (FullChromaCandidate): in eight bits, and in ten for HDR.
  bool fullTested = false;
  bool full8 = false;
  bool full10 = false;
};

// H.264 and H.265 on NVENC and the CPU. AV1, AMF and QuickSync take 4:2:0
// only, so they are not even asked.
bool FullChromaCandidate(const std::string& ffmpegName);

// All a lossless encoder is told, as key and value: nothing in the settings
// applies to one. The test encode passes the same as the recording.
std::vector<std::pair<const char*, const char*>> LosslessOptions(RecordEncoder id);

struct FfmpegInfo {
  bool found = false;   // the libraries are loaded
  std::string folder;   // where from
  std::string version;  // what the build calls itself, "9.0.2-full_build-www.gyan.dev"
  // The release number out of that, "9.0.2". Empty for a git build, which
  // carries a date or a commit count instead and so has nothing a release
  // could be compared against.
  std::string number;
  // Why the libraries in the folder did not load: a file missing, or a major
  // the program was not built for. Empty when there are none to load.
  std::string error;
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

// Where the libraries are loaded from and a download puts them: an "ffmpeg"
// folder next to qBlank.
std::filesystem::path OwnFfmpegFolder();

// Loads the libraries in OwnFfmpegFolder, or reports the ones already loaded.
// Only fills in where and which version -- the encoder probe is separate
// because it takes seconds.
FfmpegInfo LocateFfmpeg();

// "9.0.1-full_build-www.gyan.dev" -> "9.0.1". Also takes "ffmpeg version 9.0.1"
// and the "n7.1.1" some builds write. Empty unless it is a release number.
std::string FfmpegReleaseNumber(const std::string& version);

// Numerically, part by part: "9.0.10" is newer than "9.0.9", "7.1" equals
// "7.1.0". False when either side is empty.
bool IsNewerFfmpeg(const std::string& remote, const std::string& installed);

// Runs a two frame encode per candidate and marks what worked. Slow (several
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
// actually do. The same for 4:4:4 in `full8` and `full10`, unless
// `fullProbed` is false: a test from before 4:4:4 existed, which leaves it
// untested.
void ApplyCachedProbe(FfmpegInfo* info, const std::vector<int>& available, bool fullProbed,
                      const std::vector<int>& full8, const std::vector<int>& full10);

// Default recording folder: Videos\<program name>. The name can be given, which
// is what a build does after taking over the settings of an earlier name: the
// recordings stay in the folder they have always been in rather than quietly
// moving to a folder named after the new one.
std::filesystem::path DefaultRecordFolder(const std::string& name = AppNameUtf8());

}  // namespace cap
