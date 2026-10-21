#include "watermark.h"

#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cmath>

#include "u2sdk/log.h"

namespace photomode::watermark {
namespace {
template <typename T>
void SafeRelease(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

HMODULE ThisModule() {
  HMODULE h = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ThisModule), &h);
  return h;
}

// Decoder -> 32bpp RGBA (straight alpha). Factory 1, as u2sdk dev.cpp's encoder (Windows 7 without its update).
bool Decode(IWICImagingFactory* f, IWICBitmapDecoder* dec, Image* out) {
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  HRESULT hr = dec->GetFrame(0, &frame);
  if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
  if (SUCCEEDED(hr))
    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                          WICBitmapPaletteTypeCustom);
  UINT w = 0, h = 0;
  if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
  if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > 4096 || h > 4096)) hr = E_FAIL;
  if (SUCCEEDED(hr)) {
    out->w = static_cast<int>(w);
    out->h = static_cast<int>(h);
    out->rgba.resize(static_cast<size_t>(w) * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out->rgba.size()), out->rgba.data());
  }
  SafeRelease(conv);
  SafeRelease(frame);
  return SUCCEEDED(hr);
}

template <typename MakeDecoder>
bool Load(MakeDecoder make, Image* out) {
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory* f = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
            make(f, &dec) && Decode(f, dec, out);
  SafeRelease(dec);
  SafeRelease(f);
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}
}  // namespace

const char* CornerName(Corner c) {
  static const char* n[] = {"Bottom Right", "Bottom Left", "Top Left", "Top Right"};
  return n[static_cast<int>(c)];
}

Rect Place(const Mark& m, const Rect& area, int imageH, Corner c) {
  const float k = static_cast<float>(imageH) / 1080.0f;
  const float w = m.image->w * m.scale * k, h = m.image->h * m.scale * k, g = m.margin * k;
  const bool right = c == Corner::BottomRight || c == Corner::TopRight;
  const bool bottom = c == Corner::BottomRight || c == Corner::BottomLeft;
  return {right ? area.x + area.w - g - w : area.x + g, bottom ? area.y + area.h - g - h : area.y + g, w, h};
}

bool LoadResourcePng(int id, Image* out) {
  HMODULE mod = ThisModule();
  HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
  HGLOBAL data = res ? LoadResource(mod, res) : nullptr;
  const void* bytes = data ? LockResource(data) : nullptr;
  const DWORD size = res ? SizeofResource(mod, res) : 0;
  if (!bytes || !size) {
    U2LOG("watermark: resource %d missing", id);
    return false;
  }
  bool ok = Load(
      [&](IWICImagingFactory* f, IWICBitmapDecoder** dec) {
        IWICStream* s = nullptr;
        HRESULT hr = f->CreateStream(&s);
        if (SUCCEEDED(hr)) hr = s->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(bytes)), size);
        if (SUCCEEDED(hr)) hr = f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, dec);
        SafeRelease(s);
        return SUCCEEDED(hr);
      },
      out);
  U2LOG("watermark: resource %d %s (%dx%d)", id, ok ? "loaded" : "FAILED to decode", out->w, out->h);
  return ok;
}

bool LoadFilePng(const std::wstring& path, Image* out) {
  return Load(
      [&](IWICImagingFactory* f, IWICBitmapDecoder** dec) {
        return SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, dec));
      },
      out);
}

std::wstring ModuleDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(ThisModule(), buf, MAX_PATH);
  std::wstring p = buf;
  return p.substr(0, p.find_last_of(L"\\/") + 1);
}

// ---- overlay --------------------------------------------------------------------------------------
// The states recorded once into a state block: Capture() before the draw and Apply() after put the game's values back
// (the device is a PUREDEVICE, nothing can be read back one by one; accum.cpp does the same).
namespace {
void SetOverlayStates(IDirect3DDevice9* dev) {
  dev->SetVertexShader(nullptr);
  dev->SetPixelShader(nullptr);
  dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
  dev->SetTexture(0, nullptr);
  dev->SetTexture(1, nullptr);
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
  dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
  dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
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
}  // namespace

bool Overlay::Ensure(IDirect3DDevice9* dev, const Image* img) {
  if (failed_) return false;
  if (tex_ && image_ == img) return true;
  SafeRelease(tex_);
  image_ = nullptr;
  HRESULT hr = dev->CreateTexture(img->w, img->h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex_, nullptr);
  D3DLOCKED_RECT lr{};
  if (SUCCEEDED(hr)) hr = tex_->LockRect(0, &lr, nullptr, 0);
  if (SUCCEEDED(hr)) {
    for (int y = 0; y < img->h; ++y) {
      auto dst = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
      const uint8_t* src = &img->rgba[static_cast<size_t>(y) * img->w * 4];
      for (int x = 0; x < img->w; ++x, dst += 4, src += 4) dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = src[3];
    }
    tex_->UnlockRect(0);
  }
  if (SUCCEEDED(hr) && !state_) {
    hr = dev->BeginStateBlock();
    if (SUCCEEDED(hr)) {
      SetOverlayStates(dev);
      hr = dev->EndStateBlock(&state_);
    }
  }
  if (FAILED(hr)) {
    U2LOG("watermark: overlay texture or state block failed (hr=0x%08lX); not shown on screen", hr);
    failed_ = true;
    Free();
    failed_ = true;
    return false;
  }
  image_ = img;
  return true;
}

void Overlay::Draw(IDirect3DDevice9* dev, const Mark& m, Corner c, const Rect& area, int imageH, int offX, int offY) {
  if (!dev || !m.image || !Ensure(dev, m.image.get())) return;
  IDirect3DSurface9 *bb = nullptr, *rt = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  Rect r = Place(m, area, imageH, c);
  r.x -= offX;
  r.y -= offY;
  if (r.x + r.w <= 0 || r.y + r.h <= 0 || r.x >= d.Width || r.y >= d.Height) {  // not in this part
    bb->Release();
    return;
  }
  // Stream 0 and the render target are not in the recorded block (accum.cpp Draw, WidescreenFix postfxcore).
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  dev->GetStreamSource(0, &vb, &offset, &stride);
  dev->GetRenderTarget(0, &rt);
  if (SUCCEEDED(state_->Capture())) {
    SetOverlayStates(dev);
    dev->SetRenderTarget(0, bb);
    D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0, 1};
    dev->SetViewport(&vp);
    dev->SetTexture(0, tex_);
    struct V { float x, y, z, rhw, u, v; };
    const float l = r.x - 0.5f, t = r.y - 0.5f, rr = r.x + r.w - 0.5f, b = r.y + r.h - 0.5f;
    const V q[4] = {{l, t, 0, 1, 0, 0}, {rr, t, 0, 1, 1, 0}, {l, b, 0, 1, 0, 1}, {rr, b, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    if (rt) dev->SetRenderTarget(0, rt);
    state_->Apply();
  }
  dev->SetStreamSource(0, vb, offset, stride);
  SafeRelease(vb);
  SafeRelease(rt);
  SafeRelease(bb);
}

void Overlay::Free() {
  SafeRelease(tex_);
  SafeRelease(state_);
  image_ = nullptr;
  failed_ = false;
}

}  // namespace photomode::watermark
