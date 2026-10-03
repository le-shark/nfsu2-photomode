#pragma once
// Off-screen supersampling for 2x photos: for the length of one Render3D call (0x5CAAD0) the 3D
// pass and the game's post FX render into a 2W x 2H colour target and depth-stencil instead of the
// backbuffer. Spec: docs/specs/2026-10-03-supersample-2x-design.md. Main thread only.
#include <cstdint>

#include "u2sdk/game.h"

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;

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
  // Draws the last 2x frame scaled down over the current render target (warm-up frames, before the
  // first sample).
  void Blit(IDirect3DDevice9* dev);

 private:
  // A 2x stand-in for one of the game's render-target textures (and its level-0 surface).
  struct Swap {
    uintptr_t texAddr, surfAddr;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
    void* savedTex = nullptr;
    void* savedSurf = nullptr;
  };
  bool CreateSwap(IDirect3DDevice9* dev, Swap& s);

  IDirect3DSurface9* rt_ = nullptr;
  IDirect3DSurface9* ds_ = nullptr;
  // Streak/glow feedback buffer, and the full-size pair bleach/overbright copy back from.
  Swap swaps_[3] = {{u2::addr::StreakTexture, u2::addr::StreakSurface},
                    {u2::addr::FullTexture0, u2::addr::FullSurface0},
                    {u2::addr::FullTexture1, u2::addr::FullSurface1}};
  int savedFsaa_ = 0;
  int w_ = 0, h_ = 0;
  bool swapped_ = false;
  void* savedSurface_ = nullptr;
  void* savedDepth_ = nullptr;
  int savedWidth_ = 0, savedHeight_ = 0;
  void* savedBackBuffer_ = nullptr;
  void* savedAutoDepth_ = nullptr;
};

}  // namespace photomode
