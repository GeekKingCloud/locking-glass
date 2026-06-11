#include "tray_icon_drawing_win32.h"
#include "../windows_resource.h"

#if defined(_WIN32)

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace locking_glass::platform::internal {

namespace {

constexpr int kTrayIconPixels = 16;

struct IconColor {
  BYTE red = 0;
  BYTE green = 0;
  BYTE blue = 0;
  BYTE alpha = 255;
};

bool EnsureGdiplusStarted() {
  static std::once_flag start_once;
  static bool started = false;
  static ULONG_PTR gdiplus_token = 0;

  std::call_once(start_once, [] {
    Gdiplus::GdiplusStartupInput input;
    started = Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr) ==
              Gdiplus::Ok;
  });

  return started;
}

std::unique_ptr<Gdiplus::Bitmap> LoadPngResource(const UINT resource_id) {
  if (!EnsureGdiplusStarted()) {
    return nullptr;
  }

  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource =
      FindResourceW(module, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
  if (resource == nullptr) {
    return nullptr;
  }

  const DWORD resource_size = SizeofResource(module, resource);
  HGLOBAL loaded_resource = LoadResource(module, resource);
  const void* resource_data = LockResource(loaded_resource);
  if (resource_size == 0 || loaded_resource == nullptr ||
      resource_data == nullptr) {
    return nullptr;
  }

  HGLOBAL resource_copy =
      GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(resource_size));
  if (resource_copy == nullptr) {
    return nullptr;
  }

  void* copy_data = GlobalLock(resource_copy);
  if (copy_data == nullptr) {
    GlobalFree(resource_copy);
    return nullptr;
  }
  std::memcpy(copy_data, resource_data, resource_size);
  GlobalUnlock(resource_copy);

  IStream* stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(resource_copy, TRUE, &stream))) {
    GlobalFree(resource_copy);
    return nullptr;
  }

  std::unique_ptr<Gdiplus::Bitmap> source(
      Gdiplus::Bitmap::FromStream(stream));
  std::unique_ptr<Gdiplus::Bitmap> result;
  if (source != nullptr && source->GetLastStatus() == Gdiplus::Ok) {
    Gdiplus::Bitmap* clone =
        source->Clone(0, 0, source->GetWidth(), source->GetHeight(),
                      PixelFormat32bppARGB);
    if (clone != nullptr && clone->GetLastStatus() == Gdiplus::Ok) {
      result.reset(clone);
    } else {
      delete clone;
    }
  }

  stream->Release();
  return result;
}

void SetPixel(std::vector<DWORD>* pixels, const int x, const int y,
              const IconColor color) {
  if (pixels == nullptr || x < 0 || y < 0 || x >= kTrayIconPixels ||
      y >= kTrayIconPixels) {
    return;
  }

  (*pixels)[static_cast<std::size_t>(y) * kTrayIconPixels +
            static_cast<std::size_t>(x)] =
      (static_cast<DWORD>(color.alpha) << 24) |
      (static_cast<DWORD>(color.red) << 16) |
      (static_cast<DWORD>(color.green) << 8) |
      static_cast<DWORD>(color.blue);
}

void FillRect(std::vector<DWORD>* pixels, const int left, const int top,
              const int right, const int bottom, const IconColor color) {
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      SetPixel(pixels, x, y, color);
    }
  }
}

void DrawLockBadge(std::vector<DWORD>* pixels, const IconColor accent,
                   const bool open_shackle) {
  const IconColor white{255, 255, 255, 255};
  FillRect(pixels, 8, 7, 14, 13, accent);
  FillRect(pixels, 10, 9, 12, 11, white);

  SetPixel(pixels, 9, 7, white);
  SetPixel(pixels, 10, 6, white);
  SetPixel(pixels, 11, 6, white);
  SetPixel(pixels, 12, 6, white);
  SetPixel(pixels, 13, 7, white);
  SetPixel(pixels, 9, 8, white);
  if (!open_shackle) {
    SetPixel(pixels, 13, 8, white);
  }
  SetPixel(pixels, 9, 9, white);
  if (!open_shackle) {
    SetPixel(pixels, 13, 9, white);
  }
  if (open_shackle) {
    SetPixel(pixels, 13, 5, white);
    SetPixel(pixels, 14, 6, white);
    SetPixel(pixels, 14, 7, white);
  }
}

UINT TrayMenuStatusResourceId(const RECT& rect, const bool locked) {
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int size = std::min(width, height);
  if (locked) {
    return size > 20 ? LOCKING_GLASS_TRAY_LOCKED_32_PNG
                     : LOCKING_GLASS_TRAY_LOCKED_16_PNG;
  }
  return size > 20 ? LOCKING_GLASS_TRAY_UNLOCKED_32_PNG
                   : LOCKING_GLASS_TRAY_UNLOCKED_16_PNG;
}

bool DrawPngResource(HDC dc, const RECT& rect, const UINT resource_id) {
  auto bitmap = LoadPngResource(resource_id);
  if (bitmap == nullptr) {
    return false;
  }

  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) {
    return false;
  }

  Gdiplus::Graphics graphics(dc);
  graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
  graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  const Gdiplus::Rect destination(rect.left, rect.top, width, height);
  return graphics.DrawImage(bitmap.get(), destination) == Gdiplus::Ok;
}

}  // namespace

HICON CreateTrayStatusIcon(const core::TrayIconState& icon) {
  (void)icon;

  HICON resource_icon = static_cast<HICON>(
      LoadImageW(GetModuleHandleW(nullptr),
                 MAKEINTRESOURCEW(LOCKING_GLASS_APP_ICON), IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                 LR_DEFAULTCOLOR));
  if (resource_icon != nullptr) {
    return resource_icon;
  }

  const IconColor accent{47, 143, 255, 255};
  const IconColor screen_fill{18, 25, 36, 255};
  std::vector<DWORD> pixels(static_cast<std::size_t>(kTrayIconPixels) *
                            static_cast<std::size_t>(kTrayIconPixels), 0);

  // Keep the shell tray icon as a stable app identity. Lock state belongs in
  // the tooltip and per-monitor menu padlocks, not in a changing app icon.
  FillRect(&pixels, 1, 2, 13, 10, accent);
  FillRect(&pixels, 2, 3, 12, 9, screen_fill);
  FillRect(&pixels, 5, 10, 9, 12, accent);
  FillRect(&pixels, 3, 12, 11, 14, accent);

  DrawLockBadge(&pixels, accent, false);

  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = kTrayIconPixels;
  header.bV5Height = -kTrayIconPixels;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bitmap_bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color_bitmap =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                       DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (color_bitmap == nullptr || bitmap_bits == nullptr) {
    if (color_bitmap != nullptr) {
      DeleteObject(color_bitmap);
    }
    return nullptr;
  }

  std::memcpy(bitmap_bits, pixels.data(), pixels.size() * sizeof(DWORD));
  HBITMAP mask_bitmap =
      CreateBitmap(kTrayIconPixels, kTrayIconPixels, 1, 1, nullptr);
  if (mask_bitmap == nullptr) {
    DeleteObject(color_bitmap);
    return nullptr;
  }

  ICONINFO icon_info{};
  icon_info.fIcon = TRUE;
  icon_info.hbmColor = color_bitmap;
  icon_info.hbmMask = mask_bitmap;
  HICON icon_handle = CreateIconIndirect(&icon_info);

  DeleteObject(color_bitmap);
  DeleteObject(mask_bitmap);
  return icon_handle;
}

bool DrawTrayMenuStatusIcon(HDC dc, const RECT& rect, const bool locked) {
  return DrawPngResource(dc, rect, TrayMenuStatusResourceId(rect, locked));
}

bool DrawOverlayStatusIcon(HDC dc, const RECT& rect, const bool locked) {
  return DrawPngResource(dc, rect, locked ? LOCKING_GLASS_OVERLAY_LOCKED_128_PNG
                                          : LOCKING_GLASS_OVERLAY_UNLOCKED_128_PNG);
}

}  // namespace locking_glass::platform::internal

#endif
