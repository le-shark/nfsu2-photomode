#pragma once
// Watermarks (user 2026-10-20): a logo in a corner of the screen while photo mode is open and in the saved photo.
// Built-in logos are PNGs inside the ASI (photomode.rc); WATERMARK / WATERMARK_<name> files next to the ASI add more
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
Rect Place(const Mark& m, int imageW, int imageH, Corner c);

// User 2026-10-21: every file next to the ASI named WATERMARK (shown as "Custom") or WATERMARK_<name> (shown as
// <name>), any case, any image type Windows decodes. The list name from a file name, or false for other files.
inline bool ListName(const std::wstring& file, std::string* name) {
  const size_t dot = file.find_last_of(L'.');
  const std::wstring stem = file.substr(0, dot);
  static const wchar_t kPrefix[] = L"watermark";
  constexpr size_t n = sizeof kPrefix / sizeof kPrefix[0] - 1;
  if (stem.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (stem[i] != kPrefix[i] && stem[i] != kPrefix[i] - L'a' + L'A') return false;
  if (stem.size() == n) {
    *name = "Custom";
    return true;
  }
  if (stem[n] != L'_' || stem.size() == n + 1) return false;
  name->clear();
  for (size_t i = n + 1; i < stem.size(); ++i)  // the menu font is ASCII
    *name += stem[i] < 0x80 ? static_cast<char>(stem[i]) : '?';
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
  // Onto the backbuffer, which shows the part of an imageW x imageH picture starting at (offX, offY) (the whole screen
  // normally; one quarter during a 2x resolve).
  void Draw(IDirect3DDevice9* dev, const Mark& m, Corner c, int imageW, int imageH, int offX, int offY);
  void Free();  // before a device reset, and when photo mode closes

 private:
  bool Ensure(IDirect3DDevice9* dev, const Image* img);
  const Image* image_ = nullptr;
  IDirect3DTexture9* tex_ = nullptr;
  IDirect3DStateBlock9* state_ = nullptr;
  bool failed_ = false;
};

}  // namespace photomode::watermark
