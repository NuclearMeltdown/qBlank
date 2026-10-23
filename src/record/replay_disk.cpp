#include "record/replay_disk.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "common.h"
#include "record/libav.h"

namespace cap {

namespace {

// Names unique to this run, across sessions: a clip of an ended session may
// still read its files while the next one writes, and what is left from
// another run is told apart by it.
std::string RunTag() {
  static const unsigned long long tag =
      (unsigned long long)std::chrono::system_clock::now().time_since_epoch().count();
  return Format("%llx-", tag);
}

std::atomic<uint64_t> g_serial{0};

void CopyProps(const AVPacket* from, AVPacket* to) {
  to->pts = from->pts;
  to->dts = from->dts;
  to->duration = from->duration;
  to->flags = from->flags;
  to->stream_index = from->stream_index;
  to->time_base = from->time_base;
  to->size = from->size;
}

}  // namespace

struct ReplayDisk::Segment {
  std::filesystem::path path;
  int64_t first = 0;     // the pts it begins with
  int64_t assigned = 0;  // handed to the thread so far, under the replay's lock
  // On the disk so far; -1 when the rest never will be.
  std::atomic<int64_t> written{0};
  std::ofstream out;  // the thread's

  ~Segment() {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

ReplayDisk::~ReplayDisk() {
  Stop();
  // The thread goes first of the members, and finishes the jobs before.
}

void ReplayDisk::Start(const std::filesystem::path& folder, int64_t split) {
  folder_ = folder;
  split_ = std::max<int64_t>(split, 1);
  failed_.store(false, std::memory_order_relaxed);
  thread_.Post([this, folder] {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
      Fail(CAP_SAID(T("Der Ordner für das Replay ließ sich nicht anlegen.",
                      "Could not create the replay's folder.")));
      return;
    }
    const std::string tag = RunTag();
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
         it.increment(ec)) {
      const std::filesystem::path& path = it->path();
      if (path.extension() != ".replay" || PathToUtf8(path.filename()).rfind(tag, 0) == 0) continue;
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
    uint64_t freeBytes = 0;
    if (DiskFreeBytes(folder, &freeBytes)) freeBytes_.store(freeBytes, std::memory_order_relaxed);
  });
}

void ReplayDisk::Stop() {
  Clear();
  if (folder_.empty()) return;
  thread_.Post([this, folder = folder_] {
    if (writing_) writing_->out.close();
    writing_.reset();
    // Only once empty: a clip being saved may still read from it.
    std::error_code ignored;
    std::filesystem::remove(folder, ignored);
  });
}

AVPacket* ReplayDisk::Put(AVPacket* packet) {
  if (segments_.empty() ||
      ((packet->flags & AV_PKT_FLAG_KEY) && packet->pts - segments_.back()->first >= split_)) {
    Pin next = std::make_shared<Segment>();
    next->path = folder_ / (RunTag() + std::to_string(g_serial.fetch_add(1)) + ".replay");
    next->first = packet->pts;
    segments_.push_back(std::move(next));
  }
  const Pin& segment = segments_.back();
  AVPacket* kept = av_->av_packet_alloc();
  if (!kept) {
    av_->av_packet_free(&packet);
    return nullptr;
  }
  CopyProps(packet, kept);
  kept->pos = segment->assigned;
  kept->opaque = segment.get();
  segment->assigned += packet->size;
  pending_.fetch_add(packet->size, std::memory_order_relaxed);
  thread_.Post([this, segment, packet] { Write(segment, packet); });
  return kept;
}

void ReplayDisk::Trim(const AVPacket* oldest) {
  // Let go of on the thread: the last one out deletes the file.
  while (segments_.size() > 1 && segments_.front().get() != oldest->opaque) {
    thread_.Post([gone = std::move(segments_.front())] {});
    segments_.pop_front();
  }
}

void ReplayDisk::Clear() {
  if (segments_.empty()) return;
  thread_.Post([gone = std::move(segments_)] {});
  segments_.clear();
}

bool ReplayDisk::failed(Said* why) const {
  if (!failed_.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(failMutex_);
  *why = failure_;
  return true;
}

AVPacket* ReplayDisk::Copy(const Libav& av, const AVPacket* kept) {
  AVPacket* copy = av.av_packet_alloc();
  if (!copy) return nullptr;
  CopyProps(kept, copy);
  copy->pos = kept->pos;
  copy->opaque = kept->opaque;
  return copy;
}

bool ReplayDisk::Load(const Libav& av, AVPacket* packet, Reader* reader) {
  const Segment* segment = static_cast<const Segment*>(packet->opaque);
  const int64_t pos = packet->pos;
  const int size = packet->size;
  // The thread is at most a moment behind: what it was handed before the clip
  // was cut, it writes first.
  for (;;) {
    const int64_t written = segment->written.load(std::memory_order_acquire);
    if (written < 0) return false;
    if (written >= pos + size) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (reader->at != segment) {
    reader->in.close();
    reader->in.clear();
    reader->in.open(segment->path, std::ios::binary);
    reader->at = segment;
  }
  AVBufferRef* buffer = av.av_buffer_allocz((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
  if (!buffer || !reader->in.is_open()) {
    av.av_buffer_unref(&buffer);
    return false;
  }
  reader->in.clear();
  reader->in.seekg(pos);
  reader->in.read(reinterpret_cast<char*>(buffer->data), size);
  if (reader->in.gcount() != size) {
    av.av_buffer_unref(&buffer);
    return false;
  }
  packet->buf = buffer;
  packet->data = buffer->data;
  packet->opaque = nullptr;
  packet->pos = -1;
  return true;
}

void ReplayDisk::Write(const Pin& segment, AVPacket* packet) {
  Segment& file = *segment;
  const int size = packet->size;
  if (writing_ != segment) {
    if (writing_) writing_->out.close();
    writing_ = segment;
    if (!failed_.load(std::memory_order_relaxed)) {
      file.out.open(file.path, std::ios::binary | std::ios::trunc);
      if (!file.out.is_open()) {
        // Its folder went away under it: the replay switched off and on again.
        std::error_code ignored;
        std::filesystem::create_directories(file.path.parent_path(), ignored);
        file.out.clear();
        file.out.open(file.path, std::ios::binary | std::ios::trunc);
      }
      uint64_t freeBytes = 0;
      if (DiskFreeBytes(file.path.parent_path(), &freeBytes)) {
        freeBytes_.store(freeBytes, std::memory_order_relaxed);
      }
    }
  }
  if (file.written.load(std::memory_order_relaxed) >= 0) {
    bool wrote = false;
    if (file.out.is_open() && !failed_.load(std::memory_order_relaxed)) {
      file.out.write(reinterpret_cast<const char*>(packet->data), size);
      // Out of the program's buffer, so a clip can read it.
      file.out.flush();
      wrote = (bool)file.out;
    }
    if (wrote) {
      file.written.fetch_add(size, std::memory_order_release);
    } else {
      if (!failed_.load(std::memory_order_relaxed)) {
        Fail(CAP_SAID(T("Das Replay konnte nicht auf die Platte schreiben.",
                        "The replay could not write to the disk.")));
      }
      file.written.store(-1, std::memory_order_release);
    }
  }
  av_->av_packet_free(&packet);
  pending_.fetch_sub(size, std::memory_order_relaxed);
}

void ReplayDisk::Fail(Said why) {
  {
    std::lock_guard<std::mutex> lock(failMutex_);
    failure_ = std::move(why);
  }
  failed_.store(true, std::memory_order_release);
}

}  // namespace cap
