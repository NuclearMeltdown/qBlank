#pragma once

// File and folder pickers that do not freeze the picture.
//
// A system's own picker is modal and talks to the window that owns it while it
// is up. Waiting for it on the thread that owns that window deadlocks: the
// picker waits for a message pump that is blocked waiting for the picker. On
// Windows there is a second reason -- the common item dialogs want an
// apartment the rest of this program deliberately does not run in.
//
// So the picker gets a thread of its own and the caller polls for the result.
// The picture keeps running while someone browses. Everything above the thread
// is here; the picker itself is ShowFileDialog, in file_dialog_win32.cpp.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cap {

class Window;

struct FileDialogRequest {
  enum class Mode { Folder, OpenFile, OpenFiles };

  Mode mode = Mode::Folder;
  std::string title;
  std::filesystem::path startPath;  // folder to open in, or a file whose folder is used
  // Label and pattern, e.g. {"Recordings", "*.mkv;*.mp4"}. Ignored for
  // Mode::Folder. Kept as separate strings on purpose: the Win32 filter format
  // is a single buffer with embedded nulls, and building that by concatenating
  // strings silently truncates at the first one.
  std::vector<std::pair<std::string, std::string>> filters;
};

class AsyncFileDialog {
 public:
  AsyncFileDialog() = default;
  ~AsyncFileDialog();

  AsyncFileDialog(const AsyncFileDialog&) = delete;
  AsyncFileDialog& operator=(const AsyncFileDialog&) = delete;

  // Returns false when one is already open. `tag` is handed back with the
  // result so the caller knows which field asked.
  bool Start(const FileDialogRequest& request, const Window* owner, int tag);

  bool busy() const { return running_.load(std::memory_order_relaxed); }
  int tag() const { return tag_; }

  // True exactly once, after the dialog closed. `out` is empty when the user
  // cancelled. `tag` receives what was passed to Start.
  bool TakeResult(std::vector<std::filesystem::path>* out, int* tag);

 private:
  // From the dialog's thread, when it closed.
  void Deliver(std::vector<std::filesystem::path> picked);

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  int tag_ = 0;

  mutable std::mutex mutex_;
  std::vector<std::filesystem::path> results_;
};

// The system's picker, run on that thread and blocking until the user is done.
// Returns what was picked, or nothing when they cancelled. Not called
// directly: AsyncFileDialog is what the interface uses.
std::vector<std::filesystem::path> ShowFileDialog(const FileDialogRequest& request,
                                                  const Window* owner);

}  // namespace cap
