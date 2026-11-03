#pragma once
// Grid (user 2026-10-21): rule-of-thirds lines on screen while photo mode is open, never in the saved photo.
// The look is the game's WHITE_LINE texture (InGameCommon 0xE5F201A7: white rows 3-4 of 8 at alpha 0xBB, no end caps;
// verified-in-binary.md "Line textures and the Showcase crop mask"), drawn as plain quads of that colour: 2 pixels
// thick at 1080 high, scaled with the screen height and kept on whole pixels, so a line only changes length with the
// photo area (Aspect Ratio, frame.h). Drawn after the watermark in the Render3D hook (under the menu) and skipped on
// the photo's resolve frames. The same quad drawer also draws the Aspect Ratio bars (solid black, in the photo too).
#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DStateBlock9;

namespace photomode::grid {

struct Line {
  int x, y, w, h;  // pixels
};

constexpr uint32_t kLineColour = 0xBBFFFFFFu;  // WHITE_LINE's texels (ARGB)

inline int Thickness(int screenH) {
  const int t = (screenH * 2 + 540) / 1080;
  return t < 1 ? 1 : t;
}

// The thirds lines, t pixels thick, of a w x h area at (x, y): two vertical lines, and the two horizontal ones in
// three pieces each around the vertical ones, so the crossings are not blended twice (brighter squares). Returns the
// count (8).
constexpr int kMaxLines = 8;
inline int Thirds(int x, int y, int w, int h, int t, Line out[kMaxLines]) {
  int vx[2], n = 0;
  for (int i = 0; i < 2; ++i) {
    vx[i] = x + (w * (i + 1) + 1) / 3 - t / 2;
    out[n++] = {vx[i], y, t, h};
  }
  for (int i = 0; i < 2; ++i) {
    const int cy = y + (h * (i + 1) + 1) / 3 - t / 2;
    out[n++] = {x, cy, vx[0] - x, t};
    out[n++] = {vx[0] + t, cy, vx[1] - vx[0] - t, t};
    out[n++] = {vx[1] + t, cy, x + w - vx[1] - t, t};
  }
  return n;
}

// Solid quads onto the backbuffer, in its pixels.
class Overlay {
 public:
  ~Overlay() { Free(); }
  void Draw(IDirect3DDevice9* dev, const Line* rects, int count, uint32_t argb);
  void Free();  // before a device reset, and when photo mode closes

 private:
  IDirect3DStateBlock9* state_ = nullptr;
  bool failed_ = false;
};

}  // namespace photomode::grid
