#include "overlay.h"

#include <windows.h>
#include <d3d9.h>

namespace photomode::overlay {

void SetStates(IDirect3DDevice9* dev, Kind kind) {
  const bool tex = kind == Kind::Textured;
  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | (tex ? D3DFVF_TEX1 : D3DFVF_DIFFUSE));
  dev->SetTexture(0, nullptr);
  dev->SetTexture(1, nullptr);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  const DWORD arg = tex ? D3DTA_TEXTURE : D3DTA_DIFFUSE;
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, arg);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, arg);
  // The game uses fixed-function texture transforms elsewhere (REFLECTEDPROJ, 0x5BC7B0 / 0x5BC890).
  dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
  dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
  dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
  dev->SetRenderState(D3DRS_SRCBLEND, tex ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
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

bool MakeBlock(IDirect3DDevice9* dev, Kind kind, IDirect3DStateBlock9** block) {
  if (*block) return true;
  if (FAILED(dev->BeginStateBlock())) return false;
  SetStates(dev, kind);
  return SUCCEEDED(dev->EndStateBlock(block));
}

Scope::Scope(IDirect3DDevice9* dev, IDirect3DStateBlock9* block, Kind kind) : dev_(dev), block_(block) {
  if (!dev || !block || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb_))) return;
  D3DSURFACE_DESC d;
  if (FAILED(bb_->GetDesc(&d))) return;
  UINT offset = 0, stride = 0;
  dev->GetStreamSource(0, &vb_, &offset, &stride);
  offset_ = offset;
  stride_ = stride;
  dev->GetRenderTarget(0, &rt_);
  if (FAILED(block->Capture())) return;
  SetStates(dev, kind);
  dev->SetRenderTarget(0, bb_);
  D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0, 1};
  dev->SetViewport(&vp);
  w_ = static_cast<int>(d.Width);
  h_ = static_cast<int>(d.Height);
  ok_ = true;
}

Scope::~Scope() {
  if (ok_) {  // our draw ran: put back what it changed
    if (rt_) dev_->SetRenderTarget(0, rt_);  // resets the viewport; Apply() restores the game's
    block_->Apply();
    dev_->SetStreamSource(0, vb_, offset_, stride_);
  }
  if (vb_) vb_->Release();
  if (rt_) rt_->Release();
  if (bb_) bb_->Release();
}

}  // namespace photomode::overlay
