#include "capture.h"

#include <d3d9.h>
#include <windows.h>

#include <cmath>
#include <new>
#include <thread>

#include "lens.h"
#include "u2sdk/dev.h"
#include "u2sdk/log.h"

namespace photomode {
namespace {
constexpr int kWarmupFrames = 3;  // let the game settle after hiding the HUD

struct SrgbTables {
  float toLinear[256];
  uint8_t toSrgb[4097];
  SrgbTables() {
    for (int i = 0; i < 256; ++i) {
      float c = i / 255.0f;
      toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int i = 0; i <= 4096; ++i) {
      float l = i / 4096.0f;
      float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1 / 2.4f) - 0.055f;
      toSrgb[i] = static_cast<uint8_t>(u2::Clamp(s * 255.0f + 0.5f, 0, 255));
    }
  }
};
const SrgbTables& Tables() {
  static SrgbTables t;
  return t;
}
}  // namespace

bool Capture::Start(IDirect3DDevice9* dev, const CaptureParams& p, std::string* error) {
  Release();
  p_ = p;
  p_.tiles = u2::ClampInt(p.tiles, 1, lens::kMaxTiles);
  if (p_.samples < 1) p_.samples = 1;

  IDirect3DSurface9* bb = nullptr;
  if (!dev || FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
    *error = "NO BACKBUFFER";
    return false;
  }
  D3DSURFACE_DESC d;
  bb->GetDesc(&d);
  bb->Release();
  width_ = static_cast<int>(d.Width);
  height_ = static_cast<int>(d.Height);

  if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_, nullptr)) ||
      FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_, nullptr))) {
    *error = "COULD NOT CREATE CAPTURE SURFACES";
    Release();
    return false;
  }
  try {
    accum_.assign(static_cast<size_t>(width_) * height_ * 3, 0.0f);
    image_.assign(static_cast<size_t>(width_) * p_.tiles * height_ * p_.tiles * 3, 0);
  } catch (const std::bad_alloc&) {
    *error = "NOT ENOUGH MEMORY FOR THIS RESOLUTION";
    Release();
    return false;
  }
  seq_ = 0;
  total_ = p_.samples * p_.tiles * p_.tiles;
  warmup_ = kWarmupFrames;
  armed_ = false;
  active_ = true;
  U2LOG("capture start: %dx%d tiles=%d samples=%d lens=%.4fm focus=%.2fm shutter=%.4fs -> %s", width_, height_,
        p_.tiles, p_.samples, p_.lensRadius, p_.focusDist, p_.shutterSec, p_.outPath.c_str());
  return true;
}

void Capture::BeginFrame(float lastFrameDt, float* nextGameSpeed) {
  *nextGameSpeed = 0;
  if (!active_) return;
  int s = seq_ % p_.samples;
  int tile = seq_ / p_.samples;

  adjust_ = u2::ProjAdjust{};
  adjust_.focusDist = p_.focusDist > 0.05f ? p_.focusDist : 0.05f;
  if (p_.lensRadius > 0) {
    float x, y;
    u2::GoldenDisc(static_cast<uint32_t>(s), static_cast<uint32_t>(p_.samples), &x, &y);
    adjust_.lensX = x * p_.lensRadius;
    adjust_.lensY = y * p_.lensRadius;
  }
  if (p_.antialias && p_.samples > 1) {
    float jx = u2::Halton(static_cast<uint32_t>(s) + 1, 2) - 0.5f;
    float jy = u2::Halton(static_cast<uint32_t>(s) + 1, 3) - 0.5f;
    adjust_.jitterX = jx * 2.0f / (width_ * p_.tiles);
    adjust_.jitterY = jy * 2.0f / (height_ * p_.tiles);
  }
  adjust_.tiles = p_.tiles;
  adjust_.tx = tile % p_.tiles;
  adjust_.ty = tile / p_.tiles;

  if (warmup_ > 0) {
    --warmup_;
    armed_ = false;
    return;
  }
  // Last frame's sample was not read back (e.g. device lost): keep the world still until it is.
  if (armed_) return;
  armed_ = true;
  // Shutter: advance the simulation by shutter/samples between consecutive samples
  // (only for 1x captures; the world can't be rewound for the next tile).
  if (p_.shutterSec > 0 && p_.tiles == 1 && s + 1 < p_.samples && lastFrameDt > 1e-4f)
    *nextGameSpeed = (p_.shutterSec / p_.samples) / lastFrameDt;
}

void Capture::OnEndScene(IDirect3DDevice9* dev) {
  if (!active_ || !armed_) return;
  armed_ = false;
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  HRESULT hr = dev->StretchRect(bb, nullptr, rt_, nullptr, D3DTEXF_NONE);
  bb->Release();
  if (FAILED(hr) || FAILED(dev->GetRenderTargetData(rt_, sys_))) {
    Abort("FRAME READBACK FAILED");
    return;
  }
  D3DLOCKED_RECT lr;
  if (FAILED(sys_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    Abort("FRAME LOCK FAILED");
    return;
  }
  const float* lin = Tables().toLinear;
  float* acc = accum_.data();
  for (int y = 0; y < height_; ++y) {
    auto row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
    float* a = acc + static_cast<size_t>(y) * width_ * 3;
    for (int x = 0; x < width_; ++x, row += 4, a += 3) {  // X8R8G8B8 = B,G,R,X in memory
      a[0] += lin[row[2]];
      a[1] += lin[row[1]];
      a[2] += lin[row[0]];
    }
  }
  sys_->UnlockRect();
  ++seq_;
  if (seq_ % p_.samples == 0) ResolveTile();
  if (seq_ >= total_) Finish();
}

void Capture::ResolveTile() {
  int tile = seq_ / p_.samples - 1;
  int tx = tile % p_.tiles, ty = tile / p_.tiles;
  float inv = 1.0f / p_.samples;
  const uint8_t* enc = Tables().toSrgb;
  size_t stride = static_cast<size_t>(width_) * p_.tiles * 3;
  for (int y = 0; y < height_; ++y) {
    const float* a = accum_.data() + static_cast<size_t>(y) * width_ * 3;
    uint8_t* dst = image_.data() + (static_cast<size_t>(ty) * height_ + y) * stride + static_cast<size_t>(tx) * width_ * 3;
    for (int i = 0; i < width_ * 3; ++i) {
      int q = static_cast<int>(u2::Clamp(a[i] * inv, 0, 1) * 4096.0f + 0.5f);
      dst[i] = enc[q];
    }
  }
  std::fill(accum_.begin(), accum_.end(), 0.0f);
}

void Capture::Finish() {
  active_ = false;
  int w = width_ * p_.tiles, h = height_ * p_.tiles;
  std::string path = p_.outPath;
  auto img = std::make_shared<std::vector<uint8_t>>(std::move(image_));
  Release();
  ++saving_;
  Notify("SAVING...");
  std::thread([this, img, w, h, path] {
    bool ok = u2::dev::SavePng(path, w, h, img->data());
    U2LOG("png %s (%dx%d): %s", path.c_str(), w, h, ok ? "saved" : "FAILED");
    std::string name = path.substr(path.find_last_of("\\/") + 1);
    Notify(ok ? "SAVED " + name : "SAVE FAILED");
    --saving_;
  }).detach();
}

void Capture::Abort(const char* reason) {
  if (!active_) return;
  U2LOG("capture aborted: %s", reason);
  active_ = false;
  Release();
  Notify(std::string("CAPTURE FAILED: ") + reason);
}

float Capture::Progress() const { return total_ ? static_cast<float>(seq_) / total_ : 0; }

bool Capture::Saving() const { return saving_ > 0; }

void Capture::Release() {
  if (rt_) { rt_->Release(); rt_ = nullptr; }
  if (sys_) { sys_->Release(); sys_ = nullptr; }
  std::vector<float>().swap(accum_);
  std::vector<uint8_t>().swap(image_);
}

void Capture::Notify(const std::string& m) {
  std::lock_guard<std::mutex> l(msgMutex_);
  message_ = m;
}

std::string Capture::TakeMessage() {
  std::lock_guard<std::mutex> l(msgMutex_);
  std::string m;
  m.swap(message_);
  return m;
}
}  // namespace photomode
