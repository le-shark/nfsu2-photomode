#include "capture.h"

#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <new>
#include <thread>

#include "frame.h"
#include "lens.h"
#include "u2sdk/d3d.h"
#include "u2sdk/dev.h"
#include "u2sdk/log.h"

namespace photomode {

bool Capture::Start(IDirect3DDevice9* dev, const CaptureParams& p, std::string* error) {
  Release();
  p_ = p;
  p_.sample.tiles = u2::ClampInt(p.sample.tiles, 1, lens::kMaxTiles);
  if (p_.sample.samples < 1) p_.sample.samples = 1;

  D3DSURFACE_DESC d;
  if (!u2::d3d::BackBufferDesc(dev, &d)) {
    *error = "Capture failed: no back buffer";
    return false;
  }
  width_ = static_cast<int>(d.Width);
  height_ = static_cast<int>(d.Height);
  gammaUse_ = gamma_.Load(dev);
  if (FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &tex_,
                                nullptr)) ||
      FAILED(tex_->GetSurfaceLevel(0, &surf_)) ||
      (gamma_.Loaded() &&
       FAILED(dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &out_, nullptr))) ||
      FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_, nullptr))) {
    *error = "Capture failed: could not create the capture surfaces";
    Release();
    return false;
  }
  p_.screenCheck = p.screenCheck && p_.sample.tiles == 1 && !p_.supersample;
  if (p_.screenCheck && FAILED(dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                       &checkRt_, nullptr))) {
    *error = "Capture failed: could not create the capture surfaces";
    Release();
    return false;
  }
  try {
    image_.assign(static_cast<size_t>(width_) * p_.sample.tiles * height_ * p_.sample.tiles * 3, 0);
  } catch (const std::bad_alloc&) {
    *error = "Capture failed: not enough memory for this resolution";
    Release();
    return false;
  }
  tile_ = done_ = warm_ = 0;
  lastPlanned_ = -1;
  resolveArmed_ = tileDone_ = checkPending_ = false;
  active_ = true;
  static const char* const kGamma[] = {"none (no WidescreenFix gamma)", "off (WidescreenFix ConsoleGamma = 0)",
                                       "WidescreenFix ConsoleGamma", "FAILED (see above): without gamma"};
  U2LOG("capture start: %dx%d tiles=%d samples=%d lens=%.4fm focus=%.2fm shutter=%.4fs gamma %s -> %s", width_, height_,
        p_.sample.tiles, p_.sample.samples, p_.sample.lensRadius, p_.sample.focusDist, p_.shutterSec,
        kGamma[static_cast<int>(gammaUse_)], p_.outPath.c_str());
  return true;
}

CaptureFrame Capture::BeginFrame(int accumCount, float lastFrameDt) {
  CaptureFrame f;
  if (!active_) return f;
  if (tileDone_) {  // the previous tile (or quarter) was read back
    tileDone_ = false;
    ++tile_;
    lastPlanned_ = -1;
    if (!p_.supersample) {  // tiles: the next one starts from an empty mean
      f.newTile = true;
      accumCount = 0;
    }
  }
  if (checkPending_) {  // repeated until its EndScene reads it
    f.check = true;
    return f;
  }
  if (p_.supersample && warm_ < kGlowWarmupFrames) {  // world still frozen; no shutter step
    ++warm_;
    f.warm = true;
    return f;
  }
  const int n = p_.sample.samples;
  done_ = accumCount < n ? accumCount : n;
  if (resolveArmed_ || accumCount >= n) {  // also repeats a resolve whose image was not drawn
    f.resolve = resolveArmed_ = true;
    if (p_.supersample) f.region = tile_;  // quarter q goes into tile slot q
    return f;
  }
  f.sample = true;
  if (p_.supersample) {  // one full view, jittered in pixels of the 2W x 2H target
    SampleParams one = p_.sample;
    one.tiles = 1;
    f.adj = SampleAdjust(one, accumCount, 0, width_ * 2, height_ * 2);
  } else {
    f.adj = SampleAdjust(p_.sample, accumCount, tile_, width_, height_);
  }
  // Shutter: advance the simulation by shutter/samples between consecutive samples (1x or
  // supersampled 2x; the world can't be rewound for the next tile). If last frame's sample did not
  // land (no 3D that frame), keep the world still until it does.
  bool landed = lastPlanned_ < 0 || accumCount > lastPlanned_;
  const bool onePass = p_.sample.tiles == 1 || p_.supersample;
  if (p_.shutterSec > 0 && onePass && landed && accumCount + 1 < n && lastFrameDt > 1e-4f)
    f.nextGameSpeed = (p_.shutterSec / n) / lastFrameDt;
  lastPlanned_ = accumCount;
  return f;
}

void Capture::ReadResolved(IDirect3DDevice9* dev) {
  if (!active_ || !resolveArmed_) return;
  IDirect3DSurface9* src = surf_;
  if (gamma_.Loaded()) {
    if (!gamma_.Apply(dev, tex_, out_)) {
      Abort("WidescreenFix gamma failed");
      return;
    }
    src = out_;
  }
  if (FAILED(dev->GetRenderTargetData(src, sys_))) {
    Abort("frame readback failed");
    return;
  }
  D3DLOCKED_RECT lr;
  if (FAILED(sys_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    Abort("frame lock failed");
    return;
  }
  // X8R8G8B8 (B,G,R,X) into the tile as RGB.
  const int tiles = p_.sample.tiles, tx = tile_ % tiles, ty = tile_ / tiles;
  const size_t stride = static_cast<size_t>(width_) * tiles * 3;
  for (int y = 0; y < height_; ++y) {
    auto row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
    uint8_t* dst = image_.data() + (static_cast<size_t>(ty) * height_ + y) * stride + static_cast<size_t>(tx) * width_ * 3;
    for (int x = 0; x < width_; ++x, row += 4, dst += 3) {
      dst[0] = row[2];
      dst[1] = row[1];
      dst[2] = row[0];
    }
  }
  sys_->UnlockRect();
  resolveArmed_ = false;
  if (tile_ + 1 < tiles * tiles) tileDone_ = true;
  else if (p_.screenCheck) checkPending_ = true;
  else Finish();
}

void Capture::CheckScreen(IDirect3DDevice9* dev) {
  if (!active_ || !checkPending_) return;
  checkPending_ = false;
  IDirect3DSurface9* bb = nullptr;
  D3DLOCKED_RECT lr;
  HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  if (SUCCEEDED(hr)) {
    hr = dev->StretchRect(bb, nullptr, checkRt_, nullptr, D3DTEXF_NONE);
    bb->Release();
  }
  if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(checkRt_, sys_);
  if (SUCCEEDED(hr)) hr = sys_->LockRect(&lr, nullptr, D3DLOCK_READONLY);
  if (FAILED(hr)) {
    U2LOG("screen check: readback failed (hr=0x%08lX)", hr);
  } else {
    // The outer pixel is left out: with FSAA WidescreenFix clears it on the screen (HideAABug).
    int worst = 0, wx = 0, wy = 0;
    double sum = 0;
    long long off = 0, n = 0;
    for (int y = 1; y < height_ - 1; ++y) {
      auto row = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
      const uint8_t* img = image_.data() + static_cast<size_t>(y) * width_ * 3;
      for (int x = 1; x < width_ - 1; ++x)
        for (int c = 0; c < 3; ++c) {
          const int d = std::abs(int(row[x * 4 + 2 - c]) - int(img[x * 3 + c]));
          sum += d;
          ++n;
          if (d > 1) ++off;
          if (d > worst) worst = d, wx = x, wy = y;
        }
    }
    sys_->UnlockRect();
    U2LOG("screen check (gamma %d): photo vs the screen after WidescreenFix, outer pixel left out: max %d at (%d,%d), "
          "mean %.4f, %lld of %lld values off by more than 1",
          static_cast<int>(gammaUse_), worst, wx, wy, n ? sum / n : 0.0, off, n);
  }
  Finish();
}

void Capture::Finish() {
  active_ = false;
  const int fullW = width_ * p_.sample.tiles, fullH = height_ * p_.sample.tiles;
  const int aspect = p_.aspect;
  const bool crop = p_.crop;
  std::string path = p_.outPath;
  const std::string note = gammaUse_ == wsfgamma::Use::Failed ? " (without WidescreenFix gamma)" : "";
  auto img = std::make_shared<std::vector<uint8_t>>(std::move(image_));
  Release();
  ++saving_;
  Notify("Saving...");
  // The crop runs on the writer thread too (9.8 ms at 7680x4320, review 2026-10-22).
  std::thread([this, img, fullW, fullH, aspect, crop, path, note] {
    int w = fullW, h = fullH;
    if (crop) {  // Crop to Ratio: keep the photo area (the same frame::Fit the bars were drawn around)
      const frame::Rect a = frame::CropInPlace(img->data(), w, h, aspect);
      if (a.w < w || a.h < h) {
        img->resize(static_cast<size_t>(a.w) * a.h * 3);
        U2LOG("crop to %s: %dx%d -> %dx%d", frame::Name(aspect), w, h, a.w, a.h);
        w = a.w;
        h = a.h;
      }
    }
    bool ok = u2::dev::SavePng(path, w, h, img->data());
    U2LOG("png %s (%dx%d): %s", path.c_str(), w, h, ok ? "saved" : "FAILED");
    std::string name = path.substr(path.find_last_of("\\/") + 1);
    Notify(ok ? "Saved " + name + note : "Save failed (see the log)");
    --saving_;
  }).detach();
}

void Capture::Abort(const char* reason) {
  if (!active_) return;
  U2LOG("capture aborted: %s", reason);
  active_ = false;
  Release();
  Notify(std::string("Capture failed: ") + reason);
}

float Capture::Progress() const {
  if (p_.supersample) return static_cast<float>(done_) / static_cast<float>(p_.sample.samples);
  const int n = p_.sample.samples, tiles = p_.sample.tiles * p_.sample.tiles;
  return static_cast<float>(tile_ * n + done_) / static_cast<float>(n * tiles);
}

bool Capture::Saving() const { return saving_ > 0; }

void Capture::Release() {
  if (surf_) { surf_->Release(); surf_ = nullptr; }
  if (tex_) { tex_->Release(); tex_ = nullptr; }
  if (out_) { out_->Release(); out_ = nullptr; }
  if (sys_) { sys_->Release(); sys_ = nullptr; }
  if (checkRt_) { checkRt_->Release(); checkRt_ = nullptr; }
  gamma_.Release();
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
