#include "supersample.h"

#include <windows.h>
#include <d3d9.h>

#include "u2sdk/game.h"
#include "u2sdk/log.h"

namespace photomode {
using u2::Var;
namespace addr = u2::addr;

bool Supersample::Create(IDirect3DDevice9* dev, int w, int h) {
  Free();
  D3DCAPS9 caps{};
  IDirect3DSurface9* bb = nullptr;
  if (!dev || FAILED(dev->GetDeviceCaps(&caps)) || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
    U2LOG("supersample: no device");
    return false;
  }
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  bb->Release();
  if (static_cast<DWORD>(w) > caps.MaxTextureWidth || static_cast<DWORD>(h) > caps.MaxTextureHeight) {
    U2LOG("supersample: %dx%d exceeds the device limit %lux%lu", w, h, caps.MaxTextureWidth, caps.MaxTextureHeight);
    return false;
  }
  // Same format as the backbuffer (the post FX use its alpha); no MSAA: supersampling replaces it.
  HRESULT hr = dev->CreateRenderTarget(w, h, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_, nullptr);
  if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds_, nullptr);
  if (FAILED(hr)) {
    U2LOG("supersample: %dx%d targets failed (hr=0x%08lX)", w, h, hr);
    Free();
    return false;
  }
  w_ = w;
  h_ = h;
  int extra = 0;
  for (Swap& s : swaps_) {
    if (!Var<void*>(s.texAddr)) continue;  // the game didn't create it (effect off): nothing to swap
    if (!CreateSwap(dev, s)) {
      Free();
      return false;
    }
    ++extra;
  }
  U2LOG("supersample: %dx%d ok (+%d effect buffers)", w, h, extra);
  return true;
}

// Same format as the game's texture, 2x its size, 1 level, render target; starts black (the game
// clears its streak buffer only at creation, 0x5BBA6E).
bool Supersample::CreateSwap(IDirect3DDevice9* dev, Swap& s) {
  auto game = static_cast<IDirect3DTexture9*>(Var<void*>(s.texAddr));
  D3DSURFACE_DESC d;
  HRESULT hr = game->GetLevelDesc(0, &d);
  if (SUCCEEDED(hr))
    hr = dev->CreateTexture(w_, h_, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &s.tex, nullptr);
  if (SUCCEEDED(hr)) hr = s.tex->GetSurfaceLevel(0, &s.surf);
  if (SUCCEEDED(hr)) hr = dev->ColorFill(s.surf, nullptr, 0);
  if (FAILED(hr)) U2LOG("supersample: effect buffer %08X failed (hr=0x%08lX)", static_cast<unsigned>(s.texAddr), hr);
  return SUCCEEDED(hr);
}

void Supersample::Free() {
  for (Swap& s : swaps_) {
    if (s.surf) s.surf->Release();
    if (s.tex) s.tex->Release();
    s.surf = nullptr;
    s.tex = nullptr;
  }
  if (rt_) {
    rt_->Release();
    rt_ = nullptr;
  }
  if (ds_) {
    ds_->Release();
    ds_ = nullptr;
  }
  w_ = h_ = 0;
}

// What Render3D 0x5CAAD0 renders into (verified-in-binary.md "High-resolution and time-step facts"):
//  - RT[0] of the RT table 0x86E768: surface 0x86E770, depth 0x86E774, width 0x86E77C,
//    height 0x86E780, bound for view 1 at 0x5CB18C..0x5CB1EA. 0x5C21E0 rewrites the size every
//    frame before Render3D (0x5C2341), so the swap happens here, at Render3D entry.
//  - [0x870850] backbuffer and [0x870854] auto depth-stencil, used directly by the post FX
//    (0x5CC8DD, 0x5CC926, 0x5CC706, 0x5CCBC8, 0x5CCC01, 0x5CCD00, 0x5CBD8B, 0x5C6AAC).
// Nothing clears view 1's target inside Render3D (the game clears after Present, 0x5D2C39).
void Supersample::Begin(IDirect3DDevice9* dev) {
  if (!Ready() || swapped_) return;
  savedSurface_ = Var<void*>(addr::RT0Surface);
  savedDepth_ = Var<void*>(addr::RT0Depth);
  savedWidth_ = Var<int>(addr::RT0Width);
  savedHeight_ = Var<int>(addr::RT0Height);
  savedBackBuffer_ = Var<void*>(addr::BackBufferSurface);
  savedAutoDepth_ = Var<void*>(addr::AutoDepthSurface);
  Var<void*>(addr::RT0Surface) = rt_;
  Var<void*>(addr::RT0Depth) = ds_;
  Var<int>(addr::RT0Width) = w_;
  Var<int>(addr::RT0Height) = h_;
  Var<void*>(addr::BackBufferSurface) = rt_;
  Var<void*>(addr::AutoDepthSurface) = ds_;
  // The light streak/overbright chain at 2x: its feedback buffer must match our depth (the lights
  // are depth-tested against [0x870854], 0x5CBBD6), and bleach/overbright copy the full-size pair
  // back over the target (0x5C6AA4, 0x5CCBFA). The half-size bloom buffers stay 1x on purpose: their
  // kernels are in source texels, so the halo keeps its size relative to the image.
  for (Swap& s : swaps_) {
    if (!s.tex) continue;
    s.savedTex = Var<void*>(s.texAddr);
    s.savedSurf = Var<void*>(s.surfAddr);
    Var<void*>(s.texAddr) = s.tex;
    Var<void*>(s.surfAddr) = s.surf;
  }
  // FSAA would pair the 1x MSAA RT 0x86F2E0 with our non-MSAA depth; supersampling replaces it.
  savedFsaa_ = Var<int>(addr::FsaaLevel);
  Var<int>(addr::FsaaLevel) = 0;
  swapped_ = true;
  dev->SetRenderTarget(0, rt_);
  dev->SetDepthStencilSurface(ds_);
  dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
}

void Supersample::End(IDirect3DDevice9* dev) {
  if (!swapped_) return;
  Var<void*>(addr::RT0Surface) = savedSurface_;
  Var<void*>(addr::RT0Depth) = savedDepth_;
  Var<int>(addr::RT0Width) = savedWidth_;
  Var<int>(addr::RT0Height) = savedHeight_;
  Var<void*>(addr::BackBufferSurface) = savedBackBuffer_;
  Var<void*>(addr::AutoDepthSurface) = savedAutoDepth_;
  for (Swap& s : swaps_) {
    if (!s.tex) continue;
    Var<void*>(s.texAddr) = s.savedTex;
    Var<void*>(s.surfAddr) = s.savedSurf;
  }
  Var<int>(addr::FsaaLevel) = savedFsaa_;
  swapped_ = false;
  // The 2D pass rebinds RT[0] itself (0x5CBF59..), but our own drawing comes first.
  dev->SetRenderTarget(0, static_cast<IDirect3DSurface9*>(savedBackBuffer_));
  dev->SetDepthStencilSurface(static_cast<IDirect3DSurface9*>(savedAutoDepth_));
}

}  // namespace photomode
