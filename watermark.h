#pragma once
// Watermarks (user 2026-10-20): a logo in a corner of the screen while photo mode is open and in the saved photo.
// Built-in logos are PNGs inside the ASI (photomode.rc); a WATERMARK.png next to the ASI is the Custom one.
// - Screen: Overlay draws it at the end of each frame (EndScene, after WidescreenFix's gamma), so it looks as it will
//   in the file.
// - Photo: Composite blends it into the finished RGB image just before the PNG is written (any size: 1x, 2x).
// Size and margin are given for a 1080-pixel-high image and scale with the image height, so the screen and the photo
// match at 1x and 2x.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DStateBlock9;

namespace photomode::watermark {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // straight (not premultiplied) alpha, rows top to bottom
};

enum class Corner { BottomRight, BottomLeft, TopLeft, TopRight, Count };
const char* CornerName(Corner c);

struct Mark {
  std::shared_ptr<const Image> image;
  float scale = 1;   // logo pixels per image pixel at 1080 high
  float margin = 0;  // gap to the image edges, in pixels at 1080 high
};

struct Rect {
  float x, y, w, h;
};
Rect Place(const Mark& m, int imageW, int imageH, Corner c);

// Blends the mark into an RGB image (3 bytes per pixel, R G B), bilinear, sRGB blending as the screen does.
void Composite(uint8_t* rgb, int w, int h, const Mark& m, Corner c);

// PNG decoding with Windows' own codec (WIC). The caller's thread must allow COM (frame code, not DllMain).
bool LoadResourcePng(int id, Image* out);
bool LoadFilePng(const std::wstring& path, Image* out);
// The folder of this ASI, with a trailing backslash.
std::wstring ModuleDir();

// The on-screen copy: a texture of the current mark, drawn over the backbuffer.
class Overlay {
 public:
  ~Overlay() { Free(); }
  void Draw(IDirect3DDevice9* dev, const Mark& m, Corner c);
  void Free();  // before a device reset, and when photo mode closes

 private:
  bool Ensure(IDirect3DDevice9* dev, const Image* img);
  const Image* image_ = nullptr;
  IDirect3DTexture9* tex_ = nullptr;
  IDirect3DStateBlock9* state_ = nullptr;
  bool failed_ = false;
};

}  // namespace photomode::watermark
