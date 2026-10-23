#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

#include "i18n.h"
#include "job_thread.h"

struct AVPacket;

namespace cap {

struct Libav;

// The replay's pictures on the disk instead of in memory, for whoever has a
// fast one: files of about a second each, begun at a keyframe and written on a
// thread of their own, so the encoders never wait for the disk. What the ring
// keeps of a picture is its packet without the data -- `pos` its place in the
// file, `opaque` the file -- and the sound stays in memory, it is small. A clip
// being saved keeps the files it reads until it is done with them.
class ReplayDisk {
 public:
  struct Segment;
  using Pin = std::shared_ptr<Segment>;

  // What a clip's muxer keeps between two Load calls.
  struct Reader {
    const Segment* at = nullptr;
    std::ifstream in;
  };

  explicit ReplayDisk(const Libav* av) : av_(av) {}
  ~ReplayDisk();
  ReplayDisk(const ReplayDisk&) = delete;
  ReplayDisk& operator=(const ReplayDisk&) = delete;

  // Everything up to Load is under the replay's lock.

  // From now on into `folder`, made on the thread; what an earlier run left
  // there goes first. A file spans at least `split` of the picture's pts.
  void Start(const std::filesystem::path& folder, int64_t split);
  // Lets every file go and the folder with them, once nothing reads them.
  void Stop();
  // Takes `packet` and returns what the ring keeps of it. Null when out of
  // memory.
  AVPacket* Put(AVPacket* packet);
  // The files before the one `oldest` is in, once the ring let go of them.
  void Trim(const AVPacket* oldest);
  // Every file of the ring.
  void Clear();
  // The files a clip of the whole ring reads.
  std::vector<Pin> PinAll() const { return {segments_.begin(), segments_.end()}; }

  // Not yet on the disk, still in memory.
  int64_t pending() const { return pending_.load(std::memory_order_relaxed); }
  // Free on its disk, as the thread last saw it; UINT64_MAX before that.
  uint64_t freeBytes() const { return freeBytes_.load(std::memory_order_relaxed); }
  // Writing failed; `why` says how.
  bool failed(Said* why) const;

  // A copy of what the ring keeps, for a clip's queue; null when out of memory.
  static AVPacket* Copy(const Libav& av, const AVPacket* kept);
  // Gives one of those its data from the file, once the thread has written
  // it. Any thread; false when it cannot be had.
  static bool Load(const Libav& av, AVPacket* packet, Reader* reader);

 private:
  // The thread's.
  void Write(const Pin& segment, AVPacket* packet);
  void Fail(Said why);

  const Libav* av_;
  std::filesystem::path folder_;
  int64_t split_ = 0;
  uint64_t serial_ = 0;
  std::deque<Pin> segments_;
  std::atomic<int64_t> pending_{0};
  std::atomic<uint64_t> freeBytes_{UINT64_MAX};

  mutable std::mutex failMutex_;
  Said failure_;  // under failMutex_
  std::atomic<bool> failed_{false};

  Pin writing_;  // the thread's: the file it has open
  JobThread thread_;
};

}  // namespace cap
