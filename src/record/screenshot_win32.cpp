// The Windows half of screenshot.h: PNG, JPEG and JPEG XR through WIC, and the
// clipboard.

#include "record/screenshot.h"

#include <DirectXPackedVector.h>
#include <wincodec.h>

#include <algorithm>
#include <vector>

#include "common_win32.h"
#include "i18n.h"
#include "window_win32.h"

namespace cap {

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
    return fail(CAP_SAID(T("Die Zwischenablage hat das Bild abgelehnt.",
                           "The clipboard rejected the image.")));
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

}  // namespace cap
