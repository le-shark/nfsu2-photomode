#include "grid.h"

#include <windows.h>
#include <d3d9.h>

#include "u2sdk/log.h"

namespace photomode::grid {
namespace {
// Untextured, vertex-coloured quads; otherwise the states of watermark.cpp's overlay (recorded once into a state
// block: Capture() before the draw and Apply() after put the game's values back, as the device is a PUREDEVICE).
void SetStates(IDirect3DDevice9* dev) {
  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
  dev->SetTexture(0, nullptr);
  dev->SetTexture(1, nullptr);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
  dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
  dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  D3DVIEWPORT9 vp{0, 0, 1, 1, 0, 1};
  dev->SetViewport(&vp);
}

constexpr DWORD kColour = D3DCOLOR_ARGB(0xBB, 0xFF, 0xFF, 0xFF);  // WHITE_LINE's texels
}  // namespace

void Overlay::Draw(IDirect3DDevice9* dev) {
  if (!dev || failed_) return;
  if (!state_) {
    HRESULT hr = dev->BeginStateBlock();
    if (SUCCEEDED(hr)) {
      SetStates(dev);
      hr = dev->EndStateBlock(&state_);
    }
    if (FAILED(hr)) {
      U2LOG("grid: state block failed (hr=0x%08lX); not shown", hr);
      failed_ = true;
      return;
    }
  }
  IDirect3DSurface9 *bb = nullptr, *rt = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  Line lines[kMaxLines];
  const int count = Thirds(0, 0, static_cast<int>(d.Width), static_cast<int>(d.Height), lines);
  // Stream 0 and the render target are not in the recorded block (watermark.cpp Draw).
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  dev->GetStreamSource(0, &vb, &offset, &stride);
  dev->GetRenderTarget(0, &rt);
  if (SUCCEEDED(state_->Capture())) {
    SetStates(dev);
    dev->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0, 1};
    dev->SetViewport(&vp);
    struct V { float x, y, z, rhw; DWORD c; };
    V q[kMaxLines * 6];
    int n = 0;
    for (int i = 0; i < count; ++i) {  // two triangles each; -0.5 puts the quad edges on pixel edges
      const Line& l = lines[i];
      const float a = l.x - 0.5f, t = l.y - 0.5f, r = l.x + l.w - 0.5f, b = l.y + l.h - 0.5f;
      const V c[6] = {{a, t, 0, 1, kColour}, {r, t, 0, 1, kColour}, {a, b, 0, 1, kColour},
                      {r, t, 0, 1, kColour}, {r, b, 0, 1, kColour}, {a, b, 0, 1, kColour}};
      for (const V& v : c) q[n++] = v;
    }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, q, sizeof(V));
    if (rt) dev->SetRenderTarget(0, rt);
    state_->Apply();
  }
  dev->SetStreamSource(0, vb, offset, stride);
  if (vb) vb->Release();
  if (rt) rt->Release();
  bb->Release();
}

void Overlay::Free() {
  if (state_) {
    state_->Release();
    state_ = nullptr;
  }
  failed_ = false;
}

}  // namespace photomode::grid
