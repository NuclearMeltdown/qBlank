#include "record/screenshot.h"

#include <shlobj.h>
#include <DirectXPackedVector.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "app_identity.h"
#include "common_win32.h"
#include "i18n.h"
#include "text_win32.h"
#include "window_win32.h"

namespace cap {
namespace {

bool FileExists(const std::wstring& path) {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

bool SaveScreenshot(const std::filesystem::path& path, const uint8_t* pixels, int width, int height,
                    ScreenshotFormat format, int jpegQuality, std::string* error) {
  auto fail = [&](const Said& said) {
    CAP_ERR("Screenshot: %s", said.logged.c_str());
    if (error) *error = said.shown;
    return false;
  };

  if (!pixels || width <= 0 || height <= 0) return fail(CAP_SAID(T("Kein Bild.", "No picture.")));

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CAP_HR(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))))) {
    return fail(CAP_SAID(T("WIC nicht verfügbar.", "WIC not available.")));
  }

  ComPtr<IWICStream> stream;
  if (FAILED(CAP_HR(factory->CreateStream(&stream))) ||
      FAILED(CAP_HR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))) {
    return fail(CAP_SAID(T("Datei konnte nicht angelegt werden.", "Could not create the file.")));
  }

  const GUID container =
      format == ScreenshotFormat::Jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;

  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(CAP_HR(factory->CreateEncoder(container, nullptr, &encoder))) ||
      FAILED(CAP_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))) {
    return fail(CAP_SAID(T("Encoder konnte nicht erstellt werden.", "Could not create the encoder.")));
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(CAP_HR(encoder->CreateNewFrame(&frame, &props)))) {
    return fail(CAP_SAID(T("Bild konnte nicht angelegt werden.", "Could not create the frame.")));
  }

  if (format == ScreenshotFormat::Jpeg && props) {
    PROPBAG2 option = {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT value;
    ::VariantInit(&value);
    value.vt = VT_R4;
    value.fltVal = (float)std::max(1, std::min(100, jpegQuality)) / 100.0f;
    props->Write(1, &option, &value);
  }

  if (FAILED(CAP_HR(frame->Initialize(props.Get()))) ||
      FAILED(CAP_HR(frame->SetSize((UINT)width, (UINT)height)))) {
    return fail(CAP_SAID(T("Bild konnte nicht angelegt werden.", "Could not create the frame.")));
  }

  // 24 bit BGR on purpose. Both the PNG and the JPEG encoder take it natively,
  // so WIC never silently substitutes a different layout behind our back -- and
  // a screenshot of opaque video has no use for an alpha channel. SetPixelFormat
  // is only a request, so the result is checked rather than trusted.
  WICPixelFormatGUID wanted = GUID_WICPixelFormat24bppBGR;
  if (FAILED(CAP_HR(frame->SetPixelFormat(&wanted)))) {
    return fail(CAP_SAID(T("Pixelformat abgelehnt.", "Pixel format rejected.")));
  }
  if (wanted != GUID_WICPixelFormat24bppBGR) {
    return fail(CAP_SAID(T("Pixelformat abgelehnt.", "Pixel format rejected.")));
  }

  // RGBA in, BGR out: drop alpha and swap the two outer channels.
  const UINT stride = (UINT)(((size_t)width * 3 + 3) & ~(size_t)3);
  std::vector<uint8_t> row((size_t)stride, 0);
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = pixels + (size_t)y * (size_t)width * 4;
    for (int x = 0; x < width; ++x) {
      row[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 2];  // B
      row[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];  // G
      row[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 0];  // R
    }
    if (FAILED(CAP_HR(frame->WritePixels(1, stride, (UINT)row.size(), row.data())))) {
      return fail(CAP_SAID(T("Schreiben fehlgeschlagen.", "Writing failed.")));
    }
  }

  if (FAILED(CAP_HR(frame->Commit())) || FAILED(CAP_HR(encoder->Commit()))) {
    return fail(CAP_SAID(T("Datei konnte nicht abgeschlossen werden.", "Could not finalise the file.")));
  }
  return true;
}

bool CopyScreenshotToClipboard(const Window* owner, const uint8_t* pixels, int width, int height,
                               std::string* error) {
  auto fail = [&](const Said& said) {
    CAP_ERR("Screenshot to the clipboard: %s", said.logged.c_str());
    if (error) *error = said.shown;
    return false;
  };

  if (!pixels || width <= 0 || height <= 0) return fail(CAP_SAID(T("Kein Bild.", "No picture.")));

  const size_t stride = ((size_t)width * 3 + 3) & ~(size_t)3;
  const size_t image = stride * (size_t)height;

  HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + image);
  if (!handle) return fail(CAP_SAID(T("Kein Speicher.", "Out of memory.")));

  uint8_t* block = (uint8_t*)::GlobalLock(handle);
  if (!block) {
    ::GlobalFree(handle);
    return fail(CAP_SAID(T("Kein Speicher.", "Out of memory.")));
  }

  BITMAPINFOHEADER* header = (BITMAPINFOHEADER*)block;
  *header = {};
  header->biSize = sizeof(BITMAPINFOHEADER);
  header->biWidth = width;
  header->biHeight = height;  // positive: the rows below run bottom up
  header->biPlanes = 1;
  header->biBitCount = 24;
  header->biCompression = BI_RGB;
  header->biSizeImage = (DWORD)image;

  uint8_t* rows = block + sizeof(BITMAPINFOHEADER);
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = pixels + (size_t)y * (size_t)width * 4;
    uint8_t* dst = rows + (size_t)(height - 1 - y) * stride;
    for (int x = 0; x < width; ++x) {
      dst[(size_t)x * 3 + 0] = src[(size_t)x * 4 + 2];  // B
      dst[(size_t)x * 3 + 1] = src[(size_t)x * 4 + 1];  // G
      dst[(size_t)x * 3 + 2] = src[(size_t)x * 4 + 0];  // R
    }
  }
  ::GlobalUnlock(handle);

  // Another program can be holding the clipboard for the moment it takes to
  // read what is on it, and the answer to that is to come back rather than to
  // report a failure the user cannot act on.
  bool opened = false;
  for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
    opened = ::OpenClipboard(owner ? NativeWindow(*owner) : nullptr) != FALSE;
    if (!opened) ::Sleep(20);
  }
  if (!opened) {
    ::GlobalFree(handle);
    return fail(CAP_SAID(T("Zwischenablage ist belegt.", "The clipboard is busy.")));
  }

  ::EmptyClipboard();
  const bool ok = ::SetClipboardData(CF_DIB, handle) != nullptr;
  ::CloseClipboard();

  // On success the clipboard owns the block; on failure it never took it.
  if (!ok) {
    ::GlobalFree(handle);
    return fail(CAP_SAID(T("Zwischenablage abgelehnt.", "The clipboard refused it.")));
  }
  return true;
}

bool SaveScreenshotHdr(const std::filesystem::path& path, const uint16_t* halfRgba, int width,
                       int height, int stride, float paperWhiteNits, std::string* error) {
  auto fail = [&](const Said& said) {
    CAP_ERR("HDR screenshot: %s", said.logged.c_str());
    if (error) *error = said.shown;
    return false;
  };
  if (!halfRgba || width <= 0 || height <= 0) return fail(CAP_SAID(T("Kein Bild.", "No picture.")));

  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CAP_HR(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))))) {
    return fail(CAP_SAID(T("WIC nicht verfügbar.", "WIC not available.")));
  }

  ComPtr<IWICStream> stream;
  if (FAILED(CAP_HR(factory->CreateStream(&stream))) ||
      FAILED(CAP_HR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))) {
    return fail(CAP_SAID(T("Datei konnte nicht angelegt werden.", "Could not create the file.")));
  }

  // JPEG XR, because it is the one container Windows ships an encoder for that
  // holds half floats -- and because Photos recognises it as HDR, which a
  // sixteen bit PNG would not.
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(CAP_HR(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &encoder))) ||
      FAILED(CAP_HR(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))) {
    return fail(CAP_SAID(T("Für HDR-Screenshots fehlt der JPEG-XR-Encoder.",
                           "The JPEG XR encoder needed for HDR screenshots is missing.")));
  }

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(CAP_HR(encoder->CreateNewFrame(&frame, &props)))) {
    return fail(CAP_SAID(T("Bild konnte nicht angelegt werden.", "Could not create the frame.")));
  }
  if (FAILED(CAP_HR(frame->Initialize(props.Get()))) ||
      FAILED(CAP_HR(frame->SetSize((UINT)width, (UINT)height)))) {
    return fail(CAP_SAID(T("Bild konnte nicht angelegt werden.", "Could not create the frame.")));
  }

  WICPixelFormatGUID wanted = GUID_WICPixelFormat64bppRGBAHalf;
  if (FAILED(CAP_HR(frame->SetPixelFormat(&wanted))) ||
      wanted != GUID_WICPixelFormat64bppRGBAHalf) {
    return fail(CAP_SAID(T("Pixelformat abgelehnt.", "Pixel format rejected.")));
  }

  // Into scRGB, whose 1.0 is eighty nits by definition. Alpha is forced opaque:
  // what comes out of the pipeline is a picture, not a layer.
  const float scale = paperWhiteNits / 80.0f;
  const uint16_t opaque = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
  const UINT rowBytes = (UINT)((size_t)width * 8);
  std::vector<uint16_t> row((size_t)width * 4, 0);

  for (int y = 0; y < height; ++y) {
    const uint16_t* src = halfRgba + (size_t)y * ((size_t)stride / sizeof(uint16_t));
    for (int x = 0; x < width; ++x) {
      for (int c = 0; c < 3; ++c) {
        const float v = DirectX::PackedVector::XMConvertHalfToFloat(src[(size_t)x * 4 + c]);
        row[(size_t)x * 4 + c] = DirectX::PackedVector::XMConvertFloatToHalf(v * scale);
      }
      row[(size_t)x * 4 + 3] = opaque;
    }
    if (FAILED(CAP_HR(frame->WritePixels(1, rowBytes, rowBytes, reinterpret_cast<BYTE*>(row.data()))))) {
      return fail(CAP_SAID(T("Schreiben fehlgeschlagen.", "Writing failed.")));
    }
  }

  if (FAILED(CAP_HR(frame->Commit())) || FAILED(CAP_HR(encoder->Commit()))) {
    return fail(CAP_SAID(T("Datei konnte nicht abgeschlossen werden.", "Could not finalise the file.")));
  }
  return true;
}

namespace {

// Linear light with 1.0 at diffuse white, onto the PQ curve in BT.2020 and into
// the ten bit packing DXGI and ffmpeg agree on. The same arithmetic the shader
// does for the recording path, in software because a still is one frame.
void HalfToPq10(const uint16_t* halfRgba, int width, int height, int stride,
                float paperWhiteNits, std::vector<uint32_t>* out) {
  out->resize((size_t)width * height);
  for (int y = 0; y < height; ++y) {
    const uint16_t* src = halfRgba + (size_t)y * ((size_t)stride / sizeof(uint16_t));
    uint32_t* dst = out->data() + (size_t)y * width;
    for (int x = 0; x < width; ++x) {
      float rgb[3];
      for (int c = 0; c < 3; ++c) {
        rgb[c] = std::max(DirectX::PackedVector::XMConvertHalfToFloat(src[(size_t)x * 4 + c]),
                          0.0f) * paperWhiteNits;
      }
      const float r = 0.627404f * rgb[0] + 0.329283f * rgb[1] + 0.043313f * rgb[2];
      const float g = 0.069097f * rgb[0] + 0.919540f * rgb[1] + 0.011362f * rgb[2];
      const float b = 0.016391f * rgb[0] + 0.088013f * rgb[1] + 0.895595f * rgb[2];

      auto pq = [](float nits) {
        const float m1 = 0.1593017578125f, m2 = 78.84375f;
        const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
        const float yv = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
        const float p = std::pow(yv, m1);
        return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
      };
      const uint32_t rc = (uint32_t)std::clamp((int)(pq(r) * 1023.0f + 0.5f), 0, 1023);
      const uint32_t gc = (uint32_t)std::clamp((int)(pq(g) * 1023.0f + 0.5f), 0, 1023);
      const uint32_t bc = (uint32_t)std::clamp((int)(pq(b) * 1023.0f + 0.5f), 0, 1023);
      // R in the low bits, as DXGI packs R10G10B10A2 -- ffmpeg calls it x2bgr10le.
      dst[x] = rc | (gc << 10) | (bc << 20) | (3u << 30);
    }
  }
}

}  // namespace

bool SaveScreenshotAvif(const std::filesystem::path& path, const std::filesystem::path& ffmpegPath,
                        const uint16_t* halfRgba, int width, int height, int stride,
                        float paperWhiteNits, std::string* error) {
  auto fail = [&](const Said& said) {
    CAP_ERR("AVIF screenshot: %s", said.logged.c_str());
    if (error) *error = said.shown;
    return false;
  };
  if (!halfRgba || width <= 0 || height <= 0) return fail(CAP_SAID(T("Kein Bild.", "No picture.")));
  if (ffmpegPath.empty()) {
    return fail(CAP_SAID(T("AVIF braucht ffmpeg. Unter Aufnahme herunterladen, oder JPEG XR wählen.",
                           "AVIF needs ffmpeg. Download it under Recording, or choose JPEG XR.")));
  }

  std::vector<uint32_t> packed;
  HalfToPq10(halfRgba, width, height, stride, paperWhiteNits, &packed);

  std::wstring args = L"-hide_banner -loglevel error -y -f rawvideo -pix_fmt x2bgr10le";
  args += L" -s " + std::to_wstring(width) + L"x" + std::to_wstring(height);
  args += L" -i pipe:0 -frames:v 1 -c:v libaom-av1 -still-picture 1 -crf 18";
  args += L" -pix_fmt yuv420p10le";
  // Without these it is a dark picture rather than an HDR one; nothing else in
  // the file says the samples are on the PQ curve.
  args += L" -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc";
  args += L" -f avif \"" + path.native() + L"\"";

  // Through a file rather than a pipe. One still is not worth the plumbing, and
  // a temporary file cannot deadlock against a process that stops reading.
  wchar_t tempDir[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tempDir);
  wchar_t tempFile[MAX_PATH] = {};
  if (!::GetTempFileNameW(tempDir, L"cvs", 0, tempFile)) {
    return fail(CAP_SAID(T("Kein Platz für die Zwischendatei.", "Nowhere to put the working file.")));
  }
  const std::wstring raw = tempFile;
  {
    HANDLE file = ::CreateFileW(raw.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return fail(CAP_SAID(T("Zwischendatei ließ sich nicht anlegen.",
                             "The working file could not be created.")));
    }
    DWORD written = 0;
    const DWORD bytes = (DWORD)(packed.size() * sizeof(uint32_t));
    const bool ok = ::WriteFile(file, packed.data(), bytes, &written, nullptr) && written == bytes;
    ::CloseHandle(file);
    if (!ok) {
      ::DeleteFileW(raw.c_str());
      return fail(CAP_SAID(T("Zwischendatei ließ sich nicht schreiben.",
                             "The working file could not be written.")));
    }
  }

  args = args.substr(0, args.find(L"-i pipe:0")) + L"-i \"" + raw + L"\" " +
         args.substr(args.find(L"-i pipe:0") + 9);

  std::wstring command = L"\"" + ffmpegPath.native() + L"\" " + args;
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};
  std::vector<wchar_t> line(command.begin(), command.end());
  line.push_back(0);
  DWORD code = 1;
  if (::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
    ::WaitForSingleObject(pi.hProcess, 60000);
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
  }
  ::DeleteFileW(raw.c_str());

  if (code != 0) {
    return fail(CAP_SAID(T("ffmpeg konnte das AVIF nicht schreiben.",
                           "ffmpeg could not write the AVIF.")));
  }
  return true;
}

std::filesystem::path DefaultScreenshotFolder(const std::string& name) {
  PWSTR pictures = nullptr;
  std::wstring folder;
  if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)) && pictures) {
    folder = pictures;
    ::CoTaskMemFree(pictures);
  }
  if (folder.empty()) folder = ExeDirectory();
  if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
  return std::filesystem::path(folder + ToWide(name));
}

std::filesystem::path MakeHdrScreenshotPath(const std::filesystem::path& folder,
                                            HdrShotFormat format) {
  const std::wstring base = MakeScreenshotPath(folder, ScreenshotFormat::Png).native();
  if (base.empty()) return base;
  return base.substr(0, base.size() - 4) + (format == HdrShotFormat::Avif ? L".avif" : L".jxr");
}

std::filesystem::path MakeScreenshotPath(const std::filesystem::path& folder,
                                         ScreenshotFormat format) {
  std::wstring dir = folder.native();
  if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  if (!EnsureFolder(dir)) return {};

  const wchar_t* ext = format == ScreenshotFormat::Jpeg ? L".jpg" : L".png";

  SYSTEMTIME st;
  ::GetLocalTime(&st);
  wchar_t stamp[64];
  swprintf_s(stamp, L"%s_%04u-%02u-%02u_%02u-%02u-%02u", kAppName, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

  // Someone holding the key down produces several shots inside one second, and
  // silently overwriting the earlier ones would be the wrong answer.
  std::wstring candidate = dir + L"\\" + stamp + ext;
  for (int n = 2; FileExists(candidate) && n < 1000; ++n) {
    candidate = dir + L"\\" + stamp + L"_" + std::to_wstring(n) + ext;
  }
  return candidate;
}

}  // namespace cap
