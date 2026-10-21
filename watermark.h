#pragma once
// Watermarks (user 2026-10-20): a logo in a corner of the screen while photo mode is open and in the saved photo.
// Built-in logos are PNGs inside the ASI (photomode.rc); the images in the "watermarks" folder next to the ASI add more
// (ListName).
// Overlay draws it into the finished 3D frame before the menu (user: never over the photo-mode menu), so the photo's
// resolve frames, read back from that same frame, carry it too: at 2x and with tiles each screen-sized part draws its
// piece of the logo placed in the whole image.
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
// In the corner of the photo area `area` (the whole image, or the Aspect Ratio frame) of an image imageH pixels
// high: size and margin scale with the image height, so a logo keeps its size whatever the ratio.
Rect Place(const Mark& m, const Rect& area, int imageH, Corner c);

// User 2026-10-21: every image in the "watermarks" folder next to the ASI (any case; no folder, no extra watermarks),
// any image type Windows decodes, listed by its file name without the extension. The list name from a file name, or
// false when nothing is left (".png").
inline bool ListName(const std::wstring& file, std::string* name) {
  const size_t dot = file.find_last_of(L'.');
  const std::wstring stem = file.substr(0, dot);
  if (stem.empty()) return false;
  name->clear();
  for (wchar_t c : stem)  // the menu font is ASCII
    *name += c < 0x80 ? static_cast<char>(c) : '?';
  return true;
}

// PNG decoding with Windows' own codec (WIC). The caller's thread must allow COM (frame code, not DllMain).
bool LoadResourcePng(int id, Image* out);
bool LoadFilePng(const std::wstring& path, Image* out);
// The folder of this ASI, with a trailing backslash.
std::wstring ModuleDir();

// The on-screen copy: a texture of the current mark, drawn over the backbuffer.
class Overlay {
 public:
  ~Overlay() { Free(); }
  // Onto the backbuffer, which shows the part of an image imageH pixels high starting at (offX, offY) (the whole
  // screen normally; one quarter during a 2x resolve); area is the photo area in that image.
  void Draw(IDirect3DDevice9* dev, const Mark& m, Corner c, const Rect& area, int imageH, int offX, int offY);
  void Free();  // before a device reset, and when photo mode closes

 private:
  bool Ensure(IDirect3DDevice9* dev, const Image* img);
  const Image* image_ = nullptr;
  IDirect3DTexture9* tex_ = nullptr;
  IDirect3DStateBlock9* state_ = nullptr;
  bool failed_ = false;
};

}  // namespace photomode::watermark
