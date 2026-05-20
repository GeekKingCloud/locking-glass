#include "tray_icon_drawing_win32.h"

#if defined(_WIN32)

#include <algorithm>
#include <cstring>
#include <vector>

namespace locking_glass::platform::internal {

namespace {

using std::max;
using std::min;

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

void FillCircle(std::vector<DWORD>* pixels, const int center_x,
                const int center_y, const int radius,
                const IconColor color) {
  const int radius_squared = radius * radius;
  for (int y = center_y - radius; y <= center_y + radius; ++y) {
    for (int x = center_x - radius; x <= center_x + radius; ++x) {
      const int dx = x - center_x;
      const int dy = y - center_y;
      if ((dx * dx) + (dy * dy) <= radius_squared) {
        SetPixel(pixels, x, y, color);
      }
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

struct PixelSurface {
  int size = 0;
  std::vector<DWORD> pixels;
};

int ScaleDesignStart(const int coordinate, const int size) {
  return (coordinate * size) / kTrayIconPixels;
}

int ScaleDesignEnd(const int coordinate, const int size) {
  return ((coordinate * size) + kTrayIconPixels - 1) / kTrayIconPixels;
}

int ScaleDesignDistance(const int dimension, const int size) {
  return max(1, ((dimension * size) + (kTrayIconPixels / 2)) / kTrayIconPixels);
}

void SetSurfacePixel(PixelSurface* surface, const int x, const int y,
                     const IconColor color) {
  if (surface == nullptr || x < 0 || y < 0 || x >= surface->size ||
      y >= surface->size) {
    return;
  }

  surface->pixels[static_cast<std::size_t>(y) *
                      static_cast<std::size_t>(surface->size) +
                  static_cast<std::size_t>(x)] =
      (static_cast<DWORD>(color.alpha) << 24) |
      (static_cast<DWORD>(color.red) << 16) |
      (static_cast<DWORD>(color.green) << 8) |
      static_cast<DWORD>(color.blue);
}

void FillSurfaceRect(PixelSurface* surface, const int left, const int top,
                     const int right, const int bottom,
                     const IconColor color) {
  if (surface == nullptr) {
    return;
  }

  const int clamped_left = max(0, left);
  const int clamped_top = max(0, top);
  const int clamped_right = min(surface->size, right);
  const int clamped_bottom = min(surface->size, bottom);
  for (int y = clamped_top; y < clamped_bottom; ++y) {
    for (int x = clamped_left; x < clamped_right; ++x) {
      SetSurfacePixel(surface, x, y, color);
    }
  }
}

void FillDesignRect(PixelSurface* surface, const int left, const int top,
                    const int right, const int bottom,
                    const IconColor color) {
  if (surface == nullptr) {
    return;
  }

  const int scaled_left = ScaleDesignStart(left, surface->size);
  const int scaled_top = ScaleDesignStart(top, surface->size);
  const int scaled_right =
      max(scaled_left + 1, ScaleDesignEnd(right, surface->size));
  const int scaled_bottom =
      max(scaled_top + 1, ScaleDesignEnd(bottom, surface->size));
  FillSurfaceRect(surface, scaled_left, scaled_top, scaled_right,
                  scaled_bottom, color);
}

void DrawDesignRectOutline(PixelSurface* surface, const int left,
                           const int top, const int right, const int bottom,
                           const IconColor color) {
  FillDesignRect(surface, left, top, right, top + 1, color);
  FillDesignRect(surface, left, bottom - 1, right, bottom, color);
  FillDesignRect(surface, left, top, left + 1, bottom, color);
  FillDesignRect(surface, right - 1, top, right, bottom, color);
}

void FillSurfaceCircle(PixelSurface* surface, const int center_x,
                       const int center_y, const int radius,
                       const IconColor color) {
  if (surface == nullptr) {
    return;
  }

  const int radius_squared = radius * radius;
  for (int y = center_y - radius; y <= center_y + radius; ++y) {
    for (int x = center_x - radius; x <= center_x + radius; ++x) {
      const int dx = x - center_x;
      const int dy = y - center_y;
      if ((dx * dx) + (dy * dy) <= radius_squared) {
        SetSurfacePixel(surface, x, y, color);
      }
    }
  }
}

IconColor BuildMenuPadlockAccent(const core::TrayPadlockIconState& icon) {
  if (icon.accent == "amber") {
    return IconColor{217, 148, 31, 255};
  }
  if (icon.accent == "emerald") {
    return IconColor{47, 157, 90, 255};
  }
  return IconColor{115, 134, 148, 255};
}

void DrawMenuPadlock(PixelSurface* surface,
                     const core::TrayPadlockIconState& icon) {
  if (surface == nullptr) {
    return;
  }

  const IconColor accent = BuildMenuPadlockAccent(icon);
  const IconColor white{255, 255, 255, 255};
  if (icon.filled) {
    FillDesignRect(surface, 4, 7, 12, 13, accent);
    FillDesignRect(surface, 7, 8, 9, 9, white);
    FillDesignRect(surface, 7, 9, 9, 12, white);
  }
  DrawDesignRectOutline(surface, 4, 7, 12, 13, accent);
  FillDesignRect(surface, 5, 5, 6, 8, accent);
  FillDesignRect(surface, 6, 4, 10, 5, accent);
  if (icon.variant == "locked") {
    FillDesignRect(surface, 10, 5, 11, 8, accent);
  } else {
    FillDesignRect(surface, 9, 3, 10, 6, accent);
    FillDesignRect(surface, 10, 4, 11, 5, accent);
  }

  if (icon.review_badge) {
    FillSurfaceCircle(
        surface, ScaleDesignStart(12, surface->size),
        ScaleDesignStart(4, surface->size),
        ScaleDesignDistance(2, surface->size), IconColor{214, 59, 72, 255});
  }
}

}  // namespace

HBITMAP CreateMenuPadlockBitmap(const core::TrayPadlockIconState& icon) {
  const int icon_size =
      max(16, max(GetSystemMetrics(SM_CXMENUCHECK),
                  GetSystemMetrics(SM_CYMENUCHECK)));
  PixelSurface surface{
      .size = icon_size,
      .pixels =
          std::vector<DWORD>(static_cast<std::size_t>(icon_size) *
                                 static_cast<std::size_t>(icon_size),
                             0),
  };
  DrawMenuPadlock(&surface, icon);

  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = icon_size;
  header.bV5Height = -icon_size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bitmap_bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP bitmap =
      CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                       DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (bitmap == nullptr || bitmap_bits == nullptr) {
    if (bitmap != nullptr) {
      DeleteObject(bitmap);
    }
    return nullptr;
  }

  std::memcpy(bitmap_bits, surface.pixels.data(),
              surface.pixels.size() * sizeof(DWORD));
  return bitmap;
}

HICON CreateTrayStatusIcon(const core::TrayIconState& icon) {
  IconColor accent{115, 134, 148, 255};
  if (icon.variant == "review") {
    accent = IconColor{217, 148, 31, 255};
  } else if (icon.variant == "locked") {
    accent = IconColor{47, 157, 90, 255};
  } else if (icon.variant == "mixed") {
    accent = IconColor{47, 143, 255, 255};
  }

  const IconColor screen_fill{18, 25, 36, 255};
  std::vector<DWORD> pixels(static_cast<std::size_t>(kTrayIconPixels) *
                            static_cast<std::size_t>(kTrayIconPixels), 0);

  // The tray icon is generated only when state changes. Keeping the state
  // variants in code avoids shipping several tiny icon assets that can drift
  // from the tray model.
  FillRect(&pixels, 1, 2, 13, 10, accent);
  FillRect(&pixels, 2, 3, 12, 9, screen_fill);
  FillRect(&pixels, 5, 10, 9, 12, accent);
  FillRect(&pixels, 3, 12, 11, 14, accent);

  DrawLockBadge(&pixels, accent,
                icon.variant == "unlocked" || icon.variant == "idle");
  if (icon.review_badge) {
    FillCircle(&pixels, 13, 3, 2, IconColor{214, 59, 72, 255});
  }

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
