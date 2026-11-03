#pragma once
// Drawing onto the current render target from the Render3D hook (watermark.cpp, grid.cpp): the screen, or a photo's
// off-screen resolve target: the states both overlays set, recorded
// once into a state block, and a scope that saves what the block does not hold. The device is a PUREDEVICE (0x50 at
// 0x5B9B89), so nothing can be read back one by one: Capture() before the draw and Apply() after put the game's values
// back, as accum.cpp does; stream 0 and the render target are saved by hand (WidescreenFix postfxcore.ixx
// SaveVertexState; DrawPrimitiveUP resets stream 0).
struct IDirect3DDevice9;
struct IDirect3DStateBlock9;
struct IDirect3DSurface9;
struct IDirect3DVertexBuffer9;

namespace photomode::overlay {

enum class Kind {
  Textured,  // XYZRHW + TEX1, premultiplied-alpha texture (ONE, INVSRCALPHA)
  Coloured,  // XYZRHW + DIFFUSE, straight-alpha vertex colour (SRCALPHA, INVSRCALPHA)
};

void SetStates(IDirect3DDevice9* dev, Kind kind);
// Records SetStates into *block once. False (logged by the caller) when the device refuses.
bool MakeBlock(IDirect3DDevice9* dev, Kind kind, IDirect3DStateBlock9** block);

// Our states on the current render target, viewport = all of it, no depth-stencil; everything back on destruction.
class Scope {
 public:
  Scope(IDirect3DDevice9* dev, IDirect3DStateBlock9* block, Kind kind);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  bool Ok() const { return ok_; }
  int Width() const { return w_; }
  int Height() const { return h_; }

 private:
  IDirect3DDevice9* dev_;
  IDirect3DStateBlock9* block_;
  IDirect3DSurface9* rt_ = nullptr;
  IDirect3DSurface9* ds_ = nullptr;
  IDirect3DVertexBuffer9* vb_ = nullptr;
  unsigned offset_ = 0, stride_ = 0;
  int w_ = 0, h_ = 0;
  bool ok_ = false;
};

}  // namespace photomode::overlay
