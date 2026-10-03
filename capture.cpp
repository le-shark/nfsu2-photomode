#include "capture.h"

#include <d3d9.h>
#include <windows.h>

#include <new>
#include <thread>

#include "lens.h"
#include "u2sdk/dev.h"
#include "u2sdk/log.h"

namespace photomode {

bool Capture::Start(IDirect3DDevice9* dev, const CaptureParams& p, std::string* error) {
  Release();
  p_ = p;
  p_.sample.tiles = u2::ClampInt(p.sample.tiles, 1, lens::kMaxTiles);
  if (p_.sample.samples < 1) p_.sample.samples = 1;

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
    image_.assign(static_cast<size_t>(width_) * p_.sample.tiles * height_ * p_.sample.tiles * 3, 0);
  } catch (const std::bad_alloc&) {
    *error = "NOT ENOUGH MEMORY FOR THIS RESOLUTION";
    Release();
    return false;
  }
  tile_ = done_ = 0;
  lastPlanned_ = -1;
  resolveArmed_ = tileDone_ = false;
  active_ = true;
  U2LOG("capture start: %dx%d tiles=%d samples=%d lens=%.4fm focus=%.2fm shutter=%.4fs -> %s", width_, height_,
        p_.sample.tiles, p_.sample.samples, p_.sample.lensRadius, p_.sample.focusDist, p_.shutterSec, p_.outPath.c_str());
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

void Capture::OnEndScene(IDirect3DDevice9* dev) {
  if (!active_ || !resolveArmed_) return;
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
  // The backbuffer already holds the gamma-corrected mean: copy X8R8G8B8 (B,G,R,X) into the tile as RGB.
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
  if (tile_ + 1 >= tiles * tiles) Finish();
  else tileDone_ = true;
}

void Capture::Finish() {
  active_ = false;
  int w = width_ * p_.sample.tiles, h = height_ * p_.sample.tiles;
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

float Capture::Progress() const {
  if (p_.supersample) return static_cast<float>(done_) / static_cast<float>(p_.sample.samples);
  const int n = p_.sample.samples, tiles = p_.sample.tiles * p_.sample.tiles;
  return static_cast<float>(tile_ * n + done_) / static_cast<float>(n * tiles);
}

bool Capture::Saving() const { return saving_ > 0; }

void Capture::Release() {
  if (rt_) { rt_->Release(); rt_ = nullptr; }
  if (sys_) { sys_->Release(); sys_ = nullptr; }
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
