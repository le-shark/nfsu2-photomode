#include "wsfgamma.h"

#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>

#include <cstring>

#include "u2sdk/log.h"

namespace photomode::wsfgamma {
namespace {
// ID3DXEffect vtable slots (Wine include/d3dx9effect.h: ID3DXBaseEffect 0-56, then ID3DXEffect; all stdcall, this first).
enum Slot {
  kRelease = 2, kGetParameterByName = 9, kGetTechniqueByName = 13, kSetTexture = 52, kSetTechnique = 58, kBegin = 63,
  kBeginPass = 64, kCommitChanges = 65, kEndPass = 66, kEnd = 67,
};
template <typename F>
F Method(void* obj, int slot) {
  return reinterpret_cast<F>((*static_cast<void***>(obj))[slot]);
}
void ComRelease(void* obj) {
  if (obj) Method<ULONG(__stdcall*)(void*)>(obj, kRelease)(obj);
}

using CreateEffectFn = HRESULT(WINAPI*)(IDirect3DDevice9*, HMODULE, const char*, const void* defines, void* include,
                                        DWORD flags, void* pool, void** effect, void** errors);

constexpr char kTechnique[] = "ConsoleGamma", kInput[] = "InputTex2D";

bool Contains(const uint8_t* d, size_t n, const char* s) {
  const size_t k = std::strlen(s);
  for (size_t i = 0; i + k <= n; ++i)
    if (std::memcmp(d + i, s, k) == 0) return true;
  return false;
}

struct Found {
  HMODULE module = nullptr;
  WORD id = 0;
};

BOOL CALLBACK OnResource(HMODULE m, LPCSTR, LPSTR name, LONG_PTR param);
Found InModule(HMODULE m) {
  Found f;
  EnumResourceNamesA(m, MAKEINTRESOURCEA(10), &OnResource, reinterpret_cast<LONG_PTR>(&f));
  return f;
}

BOOL CALLBACK OnResource(HMODULE m, LPCSTR, LPSTR name, LONG_PTR param) {
  if (!IS_INTRESOURCE(name)) return TRUE;
  HRSRC r = FindResourceA(m, name, MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
  HGLOBAL h = r ? LoadResource(m, r) : nullptr;
  const void* data = h ? LockResource(h) : nullptr;
  if (data && IsGammaEffect(data, SizeofResource(m, r))) {
    auto f = reinterpret_cast<Found*>(param);
    f->module = m;
    f->id = static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name));
    return FALSE;
  }
  return TRUE;
}

// The module that carries the effect, whatever the .asi is called. Searched once: ASIs are loaded at startup.
Found FindEffect() {
  static bool searched = false;
  static Found found;
  if (searched) return found;
  searched = true;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return found;
  MODULEENTRY32 me{};
  me.dwSize = sizeof me;
  for (BOOL ok = Module32First(snap, &me); ok && !found.module; ok = Module32Next(snap, &me)) found = InModule(me.hModule);
  CloseHandle(snap);
  return found;
}

std::string ModulePath(HMODULE m) {
  char path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
  return std::string(path, n < MAX_PATH ? n : 0);
}
}  // namespace

std::string IniPathFor(const std::string& modulePath) {
  const size_t slash = modulePath.find_last_of("\\/");
  const size_t dot = modulePath.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return modulePath + ".ini";
  return modulePath.substr(0, dot) + ".ini";
}

// A compiled effect (fx_2_0 binary: 01 09 FF FE) naming the technique and the texture RenderGamma uses.
bool IsGammaEffect(const void* data, unsigned long size) {
  const auto d = static_cast<const uint8_t*>(data);
  static const uint8_t kFx[4] = {0x01, 0x09, 0xFF, 0xFE};
  return size > 4 && std::memcmp(d, kFx, 4) == 0 && Contains(d, size, kTechnique) && Contains(d, size, kInput);
}

Use Gamma::Load(IDirect3DDevice9* dev) {
  const Found f = FindEffect();
  if (!f.module) {
    Release();
    return Use::None;
  }
  const std::string module = ModulePath(f.module);
  return LoadFrom(dev, f.module, module, IniPathFor(module));
}

Use Gamma::LoadFrom(IDirect3DDevice9* dev, void* moduleHandle, const std::string& module, const std::string& ini) {
  Release();
  const Found f = InModule(static_cast<HMODULE>(moduleHandle));
  if (!f.module) return Use::None;
  if (!ini.empty() && GetPrivateProfileIntA("GRAPHICS", "ConsoleGamma", 1, ini.c_str()) == 0) return Use::Off;
  // WidescreenFix imports d3dx9_43.dll, so it is loaded whenever its effect is.
  HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
  auto create = d3dx ? reinterpret_cast<CreateEffectFn>(GetProcAddress(d3dx, "D3DXCreateEffectFromResourceA")) : nullptr;
  if (!create) {
    U2LOG("gamma: d3dx9_43.dll D3DXCreateEffectFromResourceA not found (%s)", module.c_str());
    return Use::Failed;
  }
  void* errors = nullptr;
  HRESULT hr = create(dev, f.module, MAKEINTRESOURCEA(f.id), nullptr, nullptr, 0, nullptr, &effect_, &errors);
  ComRelease(errors);
  if (FAILED(hr) || !effect_) {
    U2LOG("gamma: WidescreenFix effect %u in %s did not load (hr=0x%08lX)", f.id, module.c_str(), hr);
    effect_ = nullptr;
    return Use::Failed;
  }
  tech_ = Method<const char*(__stdcall*)(void*, const char*)>(effect_, kGetTechniqueByName)(effect_, kTechnique);
  input_ = Method<const char*(__stdcall*)(void*, const char*, const char*)>(effect_, kGetParameterByName)(effect_, nullptr,
                                                                                                        kInput);
  // What the draw sets beside the effect's own states (Begin(0) saves and restores those).
  if (tech_ && input_ && SUCCEEDED(dev->BeginStateBlock())) {
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    D3DVIEWPORT9 vp{0, 0, 1, 1, 0, 1};
    dev->SetViewport(&vp);
    if (FAILED(dev->EndStateBlock(&state_))) state_ = nullptr;
  }
  if (!tech_ || !input_ || !state_) {
    U2LOG("gamma: WidescreenFix effect %u in %s: technique %p, texture %p, state block %p", f.id, module.c_str(), tech_,
          input_, state_);
    Release();
    return Use::Failed;
  }
  U2LOG("gamma: WidescreenFix ConsoleGamma from %s (effect %u, %s)", module.c_str(), f.id, ini.c_str());
  return Use::On;
}

bool Gamma::Apply(IDirect3DDevice9* dev, IDirect3DTexture9* src, IDirect3DSurface9* dst) {
  if (!effect_) return false;
  D3DSURFACE_DESC d;
  if (FAILED(dst->GetDesc(&d))) return false;
  IDirect3DVertexBuffer9* vb = nullptr;
  UINT offset = 0, stride = 0;
  IDirect3DSurface9 *rt = nullptr, *ds = nullptr;
  dev->GetStreamSource(0, &vb, &offset, &stride);
  dev->GetRenderTarget(0, &rt);
  dev->GetDepthStencilSurface(&ds);
  bool ok = SUCCEEDED(state_->Capture());
  if (ok) {
    dev->SetRenderTarget(0, dst);  // the viewport becomes all of dst
    dev->SetDepthStencilSurface(nullptr);  // the auto depth-stencil may be multisampled (FSAA); Z is not used
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    Method<HRESULT(__stdcall*)(void*, const char*, IDirect3DBaseTexture9*)>(effect_, kSetTexture)(effect_, input_, src);
    Method<HRESULT(__stdcall*)(void*, const char*)>(effect_, kSetTechnique)(effect_, tech_);
    Method<HRESULT(__stdcall*)(void*)>(effect_, kCommitChanges)(effect_);
    UINT passes = 0;
    ok = SUCCEEDED(Method<HRESULT(__stdcall*)(void*, UINT*, DWORD)>(effect_, kBegin)(effect_, &passes, 0));
    if (ok) {
      ok = SUCCEEDED(Method<HRESULT(__stdcall*)(void*, UINT)>(effect_, kBeginPass)(effect_, 0));
      if (ok) {
        struct V { float x, y, z, rhw, u, v; };
        const float w = static_cast<float>(d.Width) - 0.5f, h = static_cast<float>(d.Height) - 0.5f;
        const V q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {-0.5f, h, 0, 1, 0, 1}, {w, -0.5f, 0, 1, 1, 0}, {w, h, 0, 1, 1, 1}};
        ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V)));
        Method<HRESULT(__stdcall*)(void*)>(effect_, kEndPass)(effect_);
      }
      Method<HRESULT(__stdcall*)(void*)>(effect_, kEnd)(effect_);
    }
    Method<HRESULT(__stdcall*)(void*, const char*, IDirect3DBaseTexture9*)>(effect_, kSetTexture)(effect_, input_, nullptr);
    dev->SetRenderTarget(0, rt);
    dev->SetDepthStencilSurface(ds);
    state_->Apply();
  }
  dev->SetStreamSource(0, vb, offset, stride);
  if (vb) vb->Release();
  if (rt) rt->Release();
  if (ds) ds->Release();
  return ok;
}

void Gamma::Release() {
  ComRelease(effect_);
  effect_ = nullptr;
  tech_ = input_ = nullptr;
  if (state_) {
    state_->Release();
    state_ = nullptr;
  }
}

}  // namespace photomode::wsfgamma
