#pragma once
// Off-screen supersampling for 2x photos: for the length of one Render3D call (0x5CAAD0) the 3D
// pass and the game's post FX render into a 2W x 2H colour target and depth-stencil instead of the
// backbuffer. Spec: docs/specs/2026-10-03-supersample-2x-design.md. Main thread only.
struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace photomode {

class Supersample {
 public:
  // Creates the targets (w x h, backbuffer format, no MSAA, plus D24S8). Logs and returns false
  // when the device can't hold them.
  bool Create(IDirect3DDevice9* dev, int w, int h);
  void Free();  // capture end, exit, device reset
  bool Ready() const { return rt_ && ds_; }
  int Width() const { return w_; }
  int Height() const { return h_; }
  // Render3D entry: point the game at the big targets and clear them. End restores everything;
  // both always run as a pair inside the same Render3D call.
  void Begin(IDirect3DDevice9* dev);
  void End(IDirect3DDevice9* dev);
  bool Swapped() const { return swapped_; }

 private:
  IDirect3DSurface9* rt_ = nullptr;
  IDirect3DSurface9* ds_ = nullptr;
  int w_ = 0, h_ = 0;
  bool swapped_ = false;
  void* savedSurface_ = nullptr;
  void* savedDepth_ = nullptr;
  int savedWidth_ = 0, savedHeight_ = 0;
  void* savedBackBuffer_ = nullptr;
  void* savedAutoDepth_ = nullptr;
};

}  // namespace photomode
