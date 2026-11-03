#include "watermark.h"

#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <cmath>

#include "overlay.h"
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

bool LoadResourceImage(int id, Image* out) {
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

bool LoadFileImage(const std::wstring& path, Image* out) {
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
// Uploaded with premultiplied alpha and blended ONE / INVSRCALPHA (overlay.h): the logos' fully transparent texels are
// black (nfs.png: all 21778 of them), and with straight alpha, bilinear scaling (every 2x photo, screens not 1080
// high) mixed that black into the light edge texels as a dark fringe (review 2026-10-22).
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
      for (int x = 0; x < img->w; ++x, dst += 4, src += 4) {
        const unsigned a = src[3];
        dst[0] = static_cast<uint8_t>((src[2] * a + 127) / 255);
        dst[1] = static_cast<uint8_t>((src[1] * a + 127) / 255);
        dst[2] = static_cast<uint8_t>((src[0] * a + 127) / 255);
        dst[3] = static_cast<uint8_t>(a);
      }
    }
    tex_->UnlockRect(0);
  }
  if (SUCCEEDED(hr) && !overlay::MakeBlock(dev, overlay::Kind::Textured, &state_)) hr = E_FAIL;
  if (FAILED(hr)) {
    U2LOG("watermark: overlay texture or state block failed (hr=0x%08lX); not shown on screen", hr);
    Release();
    failed_ = true;
    return false;
  }
  image_ = img;
  return true;
}

void Overlay::Draw(IDirect3DDevice9* dev, const Mark& m, Corner c, const Rect& area, int imageH, int offX, int offY) {
  if (!dev || !m.image || !Ensure(dev, m.image.get())) return;
  overlay::Scope scope(dev, state_, overlay::Kind::Textured);
  if (!scope.Ok()) return;
  Rect r = Place(m, area, imageH, c);
  r.x -= offX;
  r.y -= offY;
  if (r.x + r.w <= 0 || r.y + r.h <= 0 || r.x >= scope.Width() || r.y >= scope.Height()) return;  // not in this part
  dev->SetTexture(0, tex_);
  struct V { float x, y, z, rhw, u, v; };
  const float l = r.x - 0.5f, t = r.y - 0.5f, rr = r.x + r.w - 0.5f, b = r.y + r.h - 0.5f;
  const V q[4] = {{l, t, 0, 1, 0, 0}, {rr, t, 0, 1, 1, 0}, {l, b, 0, 1, 0, 1}, {rr, b, 0, 1, 1, 1}};
  dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
}

void Overlay::Release() {
  SafeRelease(tex_);
  SafeRelease(state_);
  image_ = nullptr;
}

void Overlay::Free() {
  Release();
  failed_ = false;
}

}  // namespace photomode::watermark
