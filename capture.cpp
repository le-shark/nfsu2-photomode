#include "capture.h"

#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <new>
#include <thread>

#include "edges.h"
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
  edges::Piece layout[edges::kPieces];
  edgeFrames_ = d.MultiSampleType != D3DMULTISAMPLE_NONE && edges::Layout(width_, height_, layout);
  edgeW_ = edgeH_ = 0;
  if (edgeFrames_)
    for (const edges::Piece& pc : layout) {
      edgeW_ = (std::max)(edgeW_, pc.dx + pc.w);
      edgeH_ = (std::max)(edgeH_, pc.dy + pc.h);
    }

  if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_, nullptr)) ||
      FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_, nullptr)) ||
      (edgeFrames_ &&
       (FAILED(dev->CreateRenderTarget(edgeW_, edgeH_, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &edgeRt_,
                                       nullptr)) ||
        FAILED(dev->CreateOffscreenPlainSurface(edgeW_, edgeH_, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &edgeSys_,
                                                nullptr))))) {
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
  resolveArmed_ = tileDone_ = edgePending_ = false;
  active_ = true;
  U2LOG("capture start: %dx%d tiles=%d samples=%d lens=%.4fm focus=%.2fm shutter=%.4fs%s -> %s", width_, height_,
        p_.sample.tiles, p_.sample.samples, p_.sample.lensRadius, p_.sample.focusDist, p_.shutterSec,
        edgeFrames_ ? " fsaa-edges" : "", p_.outPath.c_str());
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
  if (p_.supersample && warm_ < kGlowWarmupFrames) {  // world still frozen; no shutter step
    ++warm_;
    f.warm = true;
    return f;
  }
  const int n = p_.sample.samples;
  done_ = accumCount < n ? accumCount : n;
  if (edgePending_) {  // the tile's edge frame (edges.h); repeated until its EndScene reads it
    f.resolve = resolveArmed_ = f.edges = true;
    if (p_.supersample) f.region = tile_;
    return f;
  }
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
  if (edgePending_) return ReadEdges(dev);
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  HRESULT hr = dev->StretchRect(bb, nullptr, rt_, nullptr, D3DTEXF_NONE);
  bb->Release();
  if (FAILED(hr) || FAILED(dev->GetRenderTargetData(rt_, sys_))) {
    Abort("frame readback failed");
    return;
  }
  D3DLOCKED_RECT lr;
  if (FAILED(sys_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    Abort("frame lock failed");
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
  if (edgeFrames_) {
    edgePending_ = true;  // the next frame shows the edge pieces (BeginFrame)
    return;
  }
  TileRead();
}

void Capture::TileRead() {
  const int tiles = p_.sample.tiles;
  if (tile_ + 1 >= tiles * tiles) Finish();
  else tileDone_ = true;
}

// The edge frame (edges.h): the pieces at their inside places; each pixel goes back to its edge in this tile, inside
// the photo area only (the Aspect Ratio bars stay as the normal frame drew them).
void Capture::ReadEdges(IDirect3DDevice9* dev) {
  // Only the rectangle the pieces cover (about a quarter of the screen): the full-screen readback cost 2.9 ms at 1080p
  // and 12-14 ms at 4K per piece (review 2026-10-22).
  IDirect3DSurface9* bb = nullptr;
  if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
  const RECT r{0, 0, edgeW_, edgeH_};
  HRESULT hr = dev->StretchRect(bb, &r, edgeRt_, nullptr, D3DTEXF_NONE);
  bb->Release();
  if (FAILED(hr) || FAILED(dev->GetRenderTargetData(edgeRt_, edgeSys_))) {
    Abort("edge readback failed");
    return;
  }
  D3DLOCKED_RECT lr;
  if (FAILED(edgeSys_->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
    Abort("edge lock failed");
    return;
  }
  edges::Piece pieces[edges::kPieces];
  edges::Layout(width_, height_, pieces);
  const int tiles = p_.sample.tiles, tx = tile_ % tiles, ty = tile_ / tiles;
  const int imgW = width_ * tiles, imgH = height_ * tiles;
  const frame::Rect area = frame::Fit(imgW, imgH, p_.aspect);
  int written = 0;
  for (const edges::Piece& pc : pieces)
    for (int y = 0; y < pc.h; ++y)
      for (int x = 0; x < pc.w; ++x) {
        const int ix = tx * width_ + pc.sx + x, iy = ty * height_ + pc.sy + y;  // in the whole image
        if (ix < area.x || iy < area.y || ix >= area.x + area.w || iy >= area.y + area.h) continue;
        const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(pc.dy + y) * lr.Pitch +
                             static_cast<size_t>(pc.dx + x) * 4;
        uint8_t* dst = image_.data() + (static_cast<size_t>(iy) * imgW + ix) * 3;
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        ++written;
      }
  edgeSys_->UnlockRect();
  if (tile_ == 0) U2LOG("capture: FSAA edge frame, %d edge pixels written back per screen piece", written);
  resolveArmed_ = edgePending_ = false;
  TileRead();
}

void Capture::Finish() {
  active_ = false;
  const int fullW = width_ * p_.sample.tiles, fullH = height_ * p_.sample.tiles;
  const int aspect = p_.aspect;
  const bool crop = p_.crop;
  std::string path = p_.outPath;
  auto img = std::make_shared<std::vector<uint8_t>>(std::move(image_));
  Release();
  ++saving_;
  Notify("Saving...");
  // The crop runs on the writer thread too (9.8 ms at 7680x4320, review 2026-10-22).
  std::thread([this, img, fullW, fullH, aspect, crop, path] {
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
    Notify(ok ? "Saved " + name : "Save failed (see the log)");
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
  if (rt_) { rt_->Release(); rt_ = nullptr; }
  if (sys_) { sys_->Release(); sys_ = nullptr; }
  if (edgeRt_) { edgeRt_->Release(); edgeRt_ = nullptr; }
  if (edgeSys_) { edgeSys_->Release(); edgeSys_ = nullptr; }
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
