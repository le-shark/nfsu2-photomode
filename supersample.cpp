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
  U2LOG("supersample: %dx%d ok", w, h);
  return true;
}

void Supersample::Free() {
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
  swapped_ = false;
  // The 2D pass rebinds RT[0] itself (0x5CBF59..), but our own drawing comes first.
  dev->SetRenderTarget(0, static_cast<IDirect3DSurface9*>(savedBackBuffer_));
  dev->SetDepthStencilSurface(static_cast<IDirect3DSurface9*>(savedAutoDepth_));
}

}  // namespace photomode
