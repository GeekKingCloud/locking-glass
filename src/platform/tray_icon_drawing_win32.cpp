#include "tray_icon_drawing_win32.h"
#include "../windows_resource.h"

#if defined(_WIN32)

#include <cstring>
#include <vector>

namespace locking_glass::platform::internal {

namespace {

constexpr int kTrayIconPixels = 16;

struct IconColor {
  BYTE red = 0;
  BYTE green = 0;
  BYTE blue = 0;
  BYTE alpha = 255;
};

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

}  // namespace locking_glass::platform::internal

#endif
