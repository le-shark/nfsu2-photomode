#include "focusprobe.h"

#include <windows.h>
#include <d3d9.h>

#include "u2sdk/d3d.h"
#include "u2sdk/game.h"
#include "u2sdk/log.h"

#include "depthcopy_ps.h"  // fxc bytecode of shaders/depthcopy.hlsl (CMakeLists.txt)

namespace photomode::probe {
namespace {
template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr uintptr_t kViewTable = 0x832DE0;  // eView[22], stride 0x70 (daylight/depth.cpp CurrentView)

bool FormatOk(IDirect3DDevice9* dev, DWORD usage, D3DFORMAT f) {
  IDirect3D9* d3d = nullptr;
  D3DDEVICE_CREATION_PARAMETERS cp{};
  D3DDISPLAYMODE dm{};
  bool ok = SUCCEEDED(dev->GetDirect3D(&d3d)) && SUCCEEDED(dev->GetCreationParameters(&cp)) &&
            SUCCEEDED(dev->GetDisplayMode(0, &dm)) &&
            SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, usage, D3DRTYPE_TEXTURE, f));
  SafeRelease(d3d);
  return ok;
}

// The copy pass's states (accum.cpp SetOurStates: the device is a PUREDEVICE, so a state block puts the game's back).
void SetStates(IDirect3DDevice9* dev) {
  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  dev->SetTexture(0, nullptr);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  D3DVIEWPORT9 vp{0, 0, 1, 1, 0, 1};
  dev->SetViewport(&vp);
}
}  // namespace

void FocusProbe::Fail(const char* what, long hr) {
  if (!failed_) U2LOG("focus probe: %s failed (hr=0x%08lX); Aim / Closest keep the box and ray method", what, hr);
  failed_ = true;
  Free();
}

bool FocusProbe::Ensure(IDirect3DDevice9* dev) {
  if (failed_ || !dev) return false;
  D3DSURFACE_DESC d;
  if (!u2::d3d::BackBufferDesc(dev, &d)) return false;
  const int w = static_cast<int>(d.Width) / 2, h = static_cast<int>(d.Height) / 2;
  if (super_.Ready() && w == w_ && h == h_) return true;
  Free();
  if (!FormatOk(dev, D3DUSAGE_DEPTHSTENCIL, kIntz) || !FormatOk(dev, D3DUSAGE_RENDERTARGET, D3DFMT_R32F)) {
    Fail("INTZ / R32F support check", 0);
    return false;
  }
  if (!super_.Create(dev, w, h, true)) {
    Fail("half-size targets", 0);
    return false;
  }
  HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &r32_, nullptr);
  if (SUCCEEDED(hr)) hr = dev->CreateOffscreenPlainSurface(w, h, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &sys_, nullptr);
  if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(reinterpret_cast<const DWORD*>(g_ps_depthcopy), &ps_);
  if (SUCCEEDED(hr)) hr = dev->BeginStateBlock();
  if (SUCCEEDED(hr)) {
    SetStates(dev);
    hr = dev->EndStateBlock(&state_);
  }
  if (FAILED(hr)) {
    Fail("depth copy resources", hr);
    return false;
  }
  w_ = w;
  h_ = h;
  U2LOG("focus probe: %dx%d ready", w, h);
  return true;
}

bool FocusProbe::Read(IDirect3DDevice9* dev, const CarPose* car, bool countTexels, Result* out) {
  *out = {};
  if (failed_ || !super_.DepthTexture() || !r32_) return false;
  // View 1's matrices and eye as this probe frame rendered them (daylight/depth.cpp CurrentView).
  auto* view = reinterpret_cast<u2::eView*>(kViewTable + 1 * 0x70);
  if (!view->PlatInfo || !view->Camera) return false;
  const auto* plat = static_cast<const uint8_t*>(view->PlatInfo);
  const float p22 = *reinterpret_cast<const float*>(plat + 0x68), p32 = *reinterpret_cast<const float*>(plat + 0x78);
  const float eyeF[3] = {view->Camera->Position.x, view->Camera->Position.y, view->Camera->Position.z};
  u2::aomath::Frame fr;
  if (!u2::aomath::BuildFrame(reinterpret_cast<const float*>(plat + 0x80), eyeF, &fr)) return false;

  // Depth -> R32F with the copy shader (accum.cpp Draw's pattern: stream 0, RT and DS are not in the block).
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  IDirect3DSurface9 *rt = nullptr, *ds = nullptr, *dst = nullptr;
  dev->GetStreamSource(0, &vb, &offset, &stride);
  dev->GetRenderTarget(0, &rt);
  dev->GetDepthStencilSurface(&ds);
  HRESULT hr = r32_->GetSurfaceLevel(0, &dst);
  if (SUCCEEDED(hr)) hr = state_->Capture();
  if (SUCCEEDED(hr)) {
    SetStates(dev);
    dev->SetRenderTarget(0, dst);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(w_), static_cast<DWORD>(h_), 0, 1};
    dev->SetViewport(&vp);
    dev->SetPixelShader(ps_);
    dev->SetTexture(0, super_.DepthTexture());
    struct V { float x, y, z, rhw, u, v; };
    const float x1 = w_ - 0.5f, y1 = h_ - 0.5f;
    const V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    dev->SetTexture(0, nullptr);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    state_->Apply();
  }
  dev->SetStreamSource(0, vb, offset, stride);
  SafeRelease(vb);
  SafeRelease(rt);
  SafeRelease(ds);
  if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(dst, sys_);  // waits for the GPU once (capture.cpp's readback)
  SafeRelease(dst);
  if (FAILED(hr)) {
    Fail("depth copy / readback", hr);
    return false;
  }
  D3DLOCKED_RECT lr;
  if (FAILED(hr = sys_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    Fail("depth lock", hr);
    return false;
  }
  const Vec3 eye{eyeF[0], eyeF[1], eyeF[2]};
  *out = Evaluate(static_cast<const float*>(lr.pBits), w_, h_, lr.Pitch / 4, fr, p22, p32, eye, car, countTexels);
  sys_->UnlockRect();
  return true;
}

void FocusProbe::Free() {
  super_.Free();
  SafeRelease(r32_);
  SafeRelease(sys_);
  SafeRelease(ps_);
  SafeRelease(state_);
  w_ = h_ = 0;
}

}  // namespace photomode::probe
