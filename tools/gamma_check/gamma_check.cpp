// Runs photo mode's WidescreenFix gamma (scripts/photomode/wsfgamma.cpp) on the real GPU, outside the game: loads the
// installed WidescreenFix .asi as a data file (none of its code runs), a grey ramp and two colour patches through its
// ConsoleGamma effect, and prints input -> output. Checks that the effect loads by content, the D3DX calls work, and
// the draw leaves the render target, the depth-stencil and the stream as they were.
//   gamma_check.exe <path to NFSUnderground2.WidescreenFix.asi>
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

#include "wsfgamma.h"

namespace {
constexpr int kW = 256, kH = 4;

int Fail(const char* what, HRESULT hr = 0) {
  std::printf("FAIL: %s (hr=0x%08lX)\n", what, hr);
  return 1;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: gamma_check <WidescreenFix .asi>\n");
    return 2;
  }
  // The game's process has it through WidescreenFix's import; here it is loaded by hand.
  if (!LoadLibraryA("d3dx9_43.dll")) return Fail("d3dx9_43.dll not installed");
  HMODULE wsf = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!wsf) return Fail("LoadLibraryEx (data file)", GetLastError());

  HWND wnd = CreateWindowA("STATIC", "gamma_check", WS_POPUP, 0, 0, kW, kH, nullptr, nullptr, nullptr, nullptr);
  IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferWidth = kW;
  pp.BackBufferHeight = kH;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.hDeviceWindow = wnd;
  IDirect3DDevice9* dev = nullptr;
  // PUREDEVICE as the game's (0x50 at 0x5B9B89: HARDWARE_VERTEXPROCESSING | PUREDEVICE).
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, 0x50, &pp, &dev);
  if (FAILED(hr)) return Fail("CreateDevice", hr);

  photomode::wsfgamma::Gamma gamma;
  const auto use = gamma.LoadFrom(dev, wsf, argv[1], "");
  std::printf("load: %d (0 none, 1 off, 2 on, 3 failed)\n", static_cast<int>(use));
  if (use != photomode::wsfgamma::Use::On) return Fail("effect not loaded");

  IDirect3DTexture9* src = nullptr;
  IDirect3DSurface9 *srcSurf = nullptr, *out = nullptr, *up = nullptr, *down = nullptr;
  if (FAILED(hr = dev->CreateTexture(kW, kH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &src, nullptr)) ||
      FAILED(hr = src->GetSurfaceLevel(0, &srcSurf)) ||
      FAILED(hr = dev->CreateRenderTarget(kW, kH, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &out, nullptr)) ||
      FAILED(hr = dev->CreateOffscreenPlainSurface(kW, kH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &up, nullptr)) ||
      FAILED(hr = dev->CreateOffscreenPlainSurface(kW, kH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &down, nullptr)))
    return Fail("surfaces", hr);
  // Row 0-1: grey ramp; row 2: red ramp; row 3: a skin-ish colour scaled.
  D3DLOCKED_RECT lr;
  up->LockRect(&lr, nullptr, 0);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      auto p = reinterpret_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4;
      const uint8_t v = static_cast<uint8_t>(x);
      if (y < 2) p[0] = p[1] = p[2] = v;
      else if (y == 2) p[0] = 0, p[1] = 0, p[2] = v;
      else p[0] = static_cast<uint8_t>(v * 0.45f), p[1] = static_cast<uint8_t>(v * 0.65f), p[2] = v;
      p[3] = 0xFF;
    }
  up->UnlockRect();
  if (FAILED(hr = dev->UpdateSurface(up, nullptr, srcSurf, nullptr))) return Fail("UpdateSurface", hr);

  IDirect3DSurface9 *rtBefore = nullptr, *dsBefore = nullptr, *rtAfter = nullptr, *dsAfter = nullptr;
  dev->GetRenderTarget(0, &rtBefore);
  dev->GetDepthStencilSurface(&dsBefore);
  dev->BeginScene();
  const bool ok = gamma.Apply(dev, src, out);
  dev->EndScene();
  dev->GetRenderTarget(0, &rtAfter);
  dev->GetDepthStencilSurface(&dsAfter);
  std::printf("apply: %s; render target %s, depth-stencil %s\n", ok ? "ok" : "FAILED",
              rtAfter == rtBefore ? "restored" : "CHANGED", dsAfter == dsBefore ? "restored" : "CHANGED");
  if (!ok || rtAfter != rtBefore || dsAfter != dsBefore) return Fail("apply");
  if (FAILED(hr = dev->GetRenderTargetData(out, down))) return Fail("GetRenderTargetData", hr);

  down->LockRect(&lr, nullptr, D3DLOCK_READONLY);
  auto px = [&](int x, int y) { return reinterpret_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4; };
  int changed = 0, monotonic = 1, prev = -1;
  for (int x = 0; x < kW; ++x) {
    const uint8_t* p = px(x, 0);
    if (p[2] != x) ++changed;
    if (p[2] < prev) monotonic = 0;
    prev = p[2];
    if (px(x, 1)[2] != p[2]) monotonic = 0;  // rows must agree: per pixel, no filtering across
  }
  std::printf("grey in -> out (R G B):\n");
  for (int x = 0; x < kW; x += 16) {
    const uint8_t* p = px(x, 0);
    std::printf("  %3d -> %3d %3d %3d\n", x, p[2], p[1], p[0]);
  }
  const uint8_t* e = px(255, 0);
  std::printf("  255 -> %3d %3d %3d\n", e[2], e[1], e[0]);
  std::printf("red 128 -> %d %d %d; tone 200 -> %d %d %d\n", px(128, 2)[2], px(128, 2)[1], px(128, 2)[0], px(200, 3)[2],
              px(200, 3)[1], px(200, 3)[0]);
  down->UnlockRect();
  std::printf("%d of 256 grey levels changed, monotonic and per pixel: %s\n", changed, monotonic ? "yes" : "NO");
  return changed > 0 && monotonic ? 0 : 1;
}
