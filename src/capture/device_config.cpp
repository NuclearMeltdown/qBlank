#include "capture/device_config.h"

#include <olectl.h>

#include "i18n.h"

#include <vector>

namespace cap {
namespace {

// The pages the filter offers. Asking the capture pin as well is tempting --
// it has a stream format page -- but handing a frame two objects means every
// page gets both, and this card's vendor page faults on being given a pin.
// qBlank picks the capture format itself anyway, so that page has nothing to
// add that is not already in the Source tab.
void CollectPages(IUnknown* object, std::vector<GUID>* pages) {
  if (!object) return;
  ComPtr<ISpecifyPropertyPages> spec;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&spec)))) return;

  CAUUID ca = {};
  if (FAILED(spec->GetPages(&ca))) return;
  for (ULONG i = 0; i < ca.cElems; ++i) pages->push_back(ca.pElems[i]);
  if (ca.pElems) ::CoTaskMemFree(ca.pElems);
}

}  // namespace

DevicePropertyPages::~DevicePropertyPages() {
  // The dialog is modal to the user, not to us. There is no supported way to
  // close it from here, so the only correct thing on shutdown is to wait.
  if (thread_.joinable()) thread_.join();
}

bool DevicePropertyPages::Available(IBaseFilter* filter) {
  std::vector<GUID> pages;
  CollectPages(filter, &pages);
  return !pages.empty();
}

bool DevicePropertyPages::Open(IBaseFilter* filter, const std::wstring& title,
                               std::string* error) {
  if (running_.load(std::memory_order_relaxed)) {
    ReportError(error, CAP_SAID(T("Der Konfigurationsdialog ist bereits offen.",
                       "The configuration dialog is already open.")));
    return false;
  }
  if (thread_.joinable()) thread_.join();  // reap the previous one
  if (!filter) {
    ReportError(error, CAP_SAID(T("Die Karte läuft nicht.", "The card is not running.")));
    return false;
  }
  if (!Available(filter)) {
    ReportError(error, CAP_SAID(T("Diese Karte bringt keinen eigenen Konfigurationsdialog mit.",
                       "This card brings no configuration dialog of its own.")));
    return false;
  }

  // The graph lives in the multithreaded apartment. The pages need an apartment
  // threaded one, because that is what a dialog with a message pump wants, so
  // they run on a thread of their own -- and the pointer goes over as it is,
  // not through the marshaller.
  //
  // That is measured rather than assumed:
  // CoMarshalInterThreadInterfaceInStream fails outright on IID_IBaseFilter,
  // because DirectShow registers no proxy/stub for its interfaces. Handed the
  // raw pointer instead, every page this card offers -- including the vendor's
  // own -- accepts the filter and the dialog comes up populated. The reference
  // below keeps the filter alive for as long as the dialog is up and is dropped
  // on this thread, so nothing crosses an apartment boundary that COM would
  // have had to arrange.
  filter_ = filter;

  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread(&DevicePropertyPages::Run, this, title);
  return true;
}

void DevicePropertyPages::Run(std::wstring title) {
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  std::vector<GUID> pages;
  CollectPages(filter_.Get(), &pages);

  if (!pages.empty()) {
    CAP_LOG("Card configuration dialog: %d pages", (int)pages.size());
    // No owner window. The obvious thing is to pass the main window, and it is
    // wrong: a modal dialog owned across a thread boundary ties the two input
    // queues together, and the frame comes up blank and takes no input but the
    // close button. Unowned, it works -- at the cost of not floating above the
    // main window, which the caller compensates for by dropping topmost while
    // this is open.
    IUnknown* object = filter_.Get();
    const HRESULT hr = ::OleCreatePropertyFrame(nullptr, 0, 0, title.c_str(), 1, &object,
                                                (ULONG)pages.size(), pages.data(), 0, 0, nullptr);
    if (FAILED(hr)) CAP_LOG("OleCreatePropertyFrame failed: 0x%08lX", (unsigned long)hr);
  } else {
    CAP_LOG("Card configuration dialog: no pages found");
  }

  if (SUCCEEDED(init)) ::CoUninitialize();
  running_.store(false, std::memory_order_release);
}

}  // namespace cap
