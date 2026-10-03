#pragma once
// GPU accumulator: a running mean of the rendered frames in linear light (fp32 ping-pong render
// targets), drawn back into the backbuffer. Called from the Render3D 0x5CAAD0 post-hook, where the
// backbuffer holds the finished 3D scene + game post FX and no FE yet (see the GPU accumulation
// spec, docs/specs/2026-10-03-gpu-accumulation-design.md). Main thread only.
struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
struct IDirect3DPixelShader9;
struct IDirect3DStateBlock9;

namespace photomode {

class Accum {
 public:
  // Creates the resources at the backbuffer size (recreated, count 0, if that size changed).
  // After a failure it returns false (logged once) until Free.
  bool Ensure(IDirect3DDevice9* dev);
  bool Failed() const { return failed_; }
  void Reset() { count_ = 0; }
  int Count() const { return count_; }
  int Width() const { return width_; }
  int Height() const { return height_; }
  // Folds the current render target (the backbuffer) into the mean.
  void AddSample(IDirect3DDevice9* dev);
  // Draws the mean into the current render target; nothing while the count is 0.
  void Show(IDirect3DDevice9* dev);
  // Keeps a copy of the current render target (the frame on screen), and draws it back later:
  // tile captures show this still frame under the live menu instead of the zoomed tiles.
  void Hold(IDirect3DDevice9* dev);
  void ShowHold(IDirect3DDevice9* dev);
  // Releases everything (count 0, failure cleared); the next Ensure recreates it. Called on exit
  // (about 82 MB at 1080p are not kept outside photo mode) and before the game's device Reset
  // (DEFAULT-pool resources must go first).
  void Free();

 private:
  bool Create(IDirect3DDevice9* dev, int w, int h);
  void Release();
  void Fail(const char* what, long hr);
  void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DPixelShader9* ps, IDirect3DTexture9* t0,
            IDirect3DTexture9* t1, float c0);

  bool failed_ = false;
  int count_ = 0, width_ = 0, height_ = 0, cur_ = 0;
  IDirect3DTexture9* scene_ = nullptr;
  IDirect3DTexture9* hold_ = nullptr;
  IDirect3DTexture9* acc_[2] = {};
  IDirect3DPixelShader9* psAccum_ = nullptr;
  IDirect3DPixelShader9* psShow_ = nullptr;
  IDirect3DPixelShader9* psCopy_ = nullptr;
  IDirect3DStateBlock9* state_ = nullptr;
};

}  // namespace photomode
