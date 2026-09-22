#include "ui/file_dialog.h"

#include "common_win32.h"

#include <shobjidl.h>

#include "text_win32.h"
#include "window_win32.h"

namespace cap {
namespace {

std::vector<std::filesystem::path> PickPaths(const FileDialogRequest& request, HWND owner);

}  // namespace

AsyncFileDialog::~AsyncFileDialog() {
  // The dialog is modal to the user, not to us: there is no way to cancel it
  // from here, so the only correct thing on shutdown is to wait for it.
  if (thread_.joinable()) thread_.join();
}

bool AsyncFileDialog::Start(const FileDialogRequest& request, const Window* owner, int tag) {
  if (running_.load(std::memory_order_relaxed)) return false;
  if (thread_.joinable()) thread_.join();  // reap the previous one

  {
    std::lock_guard<std::mutex> lock(mutex_);
    results_.clear();
  }
  tag_ = tag;
  ready_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_relaxed);
  const HWND hwnd = owner ? NativeWindow(*owner) : nullptr;
  thread_ = std::thread([this, request, hwnd] { Deliver(PickPaths(request, hwnd)); });
  return true;
}

bool AsyncFileDialog::TakeResult(std::vector<std::filesystem::path>* out, int* tag) {
  if (!ready_.load(std::memory_order_acquire)) return false;
  ready_.store(false, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
  if (out) {
    std::lock_guard<std::mutex> lock(mutex_);
    *out = results_;
  }
  if (tag) *tag = tag_;
  return true;
}

void AsyncFileDialog::Deliver(std::vector<std::filesystem::path> picked) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    results_ = std::move(picked);
  }
  running_.store(false, std::memory_order_relaxed);
  ready_.store(true, std::memory_order_release);
}

namespace {

std::vector<std::filesystem::path> PickPaths(const FileDialogRequest& request, HWND owner) {
  std::vector<std::filesystem::path> picked;

  // Apartment threaded, which is what the common item dialogs require.
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (SUCCEEDED(init)) {
    ComPtr<IFileOpenDialog> dialog;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&dialog)))) {
      DWORD options = 0;
      dialog->GetOptions(&options);
      options |= FOS_FORCEFILESYSTEM;
      if (request.mode == FileDialogRequest::Mode::Folder) {
        options |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
      } else {
        options |= FOS_FILEMUSTEXIST;
        if (request.mode == FileDialogRequest::Mode::OpenFiles) options |= FOS_ALLOWMULTISELECT;
      }
      dialog->SetOptions(options);

      if (!request.title.empty()) dialog->SetTitle(ToWide(request.title).c_str());

      if (!request.filters.empty()) {
        // The spec only points into these, so they have to outlive the call.
        std::vector<std::pair<std::wstring, std::wstring>> wide;
        wide.reserve(request.filters.size());
        for (const auto& f : request.filters) wide.emplace_back(ToWide(f.first), ToWide(f.second));
        std::vector<COMDLG_FILTERSPEC> specs;
        specs.reserve(wide.size());
        for (const auto& f : wide) {
          specs.push_back(COMDLG_FILTERSPEC{f.first.c_str(), f.second.c_str()});
        }
        dialog->SetFileTypes((UINT)specs.size(), specs.data());
      }

      if (!request.startPath.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(::SHCreateItemFromParsingName(request.startPath.c_str(), nullptr,
                                                    IID_PPV_ARGS(&item)))) {
          dialog->SetFolder(item.Get());
        }
      }

      // Cancelling returns a failure code, which is not an error worth logging.
      if (SUCCEEDED(dialog->Show(owner))) {
        ComPtr<IShellItemArray> items;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
          DWORD count = 0;
          items->GetCount(&count);
          for (DWORD i = 0; i < count; ++i) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
              picked.emplace_back(path);
              ::CoTaskMemFree(path);
            }
          }
        }
      }
    }
    ::CoUninitialize();
  }
  return picked;
}

}  // namespace
}  // namespace cap
