#pragma once
// Aspect Ratio (user 2026-10-21): the photo area inside the screen (or the whole 2x / tiled image), centred, as large
// as fits; the rest gets solid black bars, on screen and in the photo. Crop to Ratio cuts the bars off the saved PNG.
// Pure integer maths, so the screen, the 2x quarters and the crop agree to the pixel (unit-tested).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace photomode::frame {

constexpr int kCount = 11;
inline const char* Name(int i) {
  static const char* n[kCount] = {"Native", "16:9", "16:10", "2.37:1", "4:3", "3:2",
                                  "1:1",    "9:16", "10:16", "3:4",    "1:2.37"};
  return i >= 0 && i < kCount ? n[i] : n[0];
}
// Width : height as two whole numbers (2.37:1 as 237:100); Native is 0:0.
inline void Ratio(int i, int* rw, int* rh) {
  static const int r[kCount][2] = {{0, 0}, {16, 9}, {16, 10}, {237, 100}, {4, 3},  {3, 2},
                                   {1, 1}, {9, 16}, {10, 16}, {3, 4},     {100, 237}};
  const int k = i >= 0 && i < kCount ? i : 0;
  *rw = r[k][0];
  *rh = r[k][1];
}

struct Rect {
  int x, y, w, h;
};

// The photo area of a w x h image: full for Native, else the largest centred rectangle of the ratio.
inline Rect Fit(int w, int h, int i) {
  int rw, rh;
  Ratio(i, &rw, &rh);
  if (!rw) return {0, 0, w, h};
  long long fw = w, fh = (static_cast<long long>(w) * rh + rw / 2) / rw;
  if (fh > h) {
    fh = h;
    fw = (static_cast<long long>(h) * rw + rh / 2) / rh;
  }
  return {static_cast<int>((w - fw) / 2), static_cast<int>((h - fh) / 2), static_cast<int>(fw), static_cast<int>(fh)};
}

// The bars around area f in a w x h image (0, 2 or 4 rectangles, none empty). Returns the count.
inline int Bars(int w, int h, const Rect& f, Rect out[4]) {
  int n = 0;
  if (f.y > 0) out[n++] = {0, 0, w, f.y};
  if (f.y + f.h < h) out[n++] = {0, f.y + f.h, w, h - f.y - f.h};
  if (f.x > 0) out[n++] = {0, f.y, f.x, f.h};
  if (f.x + f.w < w) out[n++] = {f.x + f.w, f.y, w - f.x - f.w, f.h};
  return n;
}

// Crop to Ratio: moves the photo area of a w x h RGB image (3 bytes a pixel, rows packed) to the front, rows packed
// at the area's width, and returns the area. Rows only move forward, so memmove in place is safe.
inline Rect CropInPlace(uint8_t* px, int w, int h, int i) {
  const Rect a = Fit(w, h, i);
  if (a.w < w || a.h < h)
    for (int y = 0; y < a.h; ++y)
      std::memmove(px + static_cast<size_t>(y) * a.w * 3, px + (static_cast<size_t>(a.y + y) * w + a.x) * 3,
                   static_cast<size_t>(a.w) * 3);
  return a;
}

}  // namespace photomode::frame
