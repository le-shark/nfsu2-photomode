#pragma once
// Accumulation capture: renders N jittered frames per tile (lens position, sub-pixel offset,
// optional shutter time slice), reads each finished frame back at EndScene, averages in linear
// light on the CPU, stitches tiles and writes a PNG on a worker thread.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "u2sdk/u2math.h"

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace photomode {

struct CaptureParams {
  int samples = 128;
  int tiles = 1;            // 1..lens::kMaxTiles (image is tiles x tiles screens)
  float lensRadius = 0;     // metres, 0 = pinhole
  float focusDist = 10;     // metres
  float shutterSec = 0;     // 0 = no motion blur
  bool antialias = true;
  std::string outPath;      // full path of the .png
};

class Capture {
 public:
  ~Capture() { Release(); }

  // Allocates everything up front. On failure returns false and sets `error`.
  bool Start(IDirect3DDevice9* dev, const CaptureParams& p, std::string* error);
  bool Active() const { return active_; }

  // Called at the start of each rendered frame. Prepares the projection adjustment for this
  // frame and returns the game speed to use for the *next* simulation step.
  void BeginFrame(float lastFrameDt, float* nextGameSpeed);
  // Projection adjustment for the player view this frame (nullptr = none).
  const u2::ProjAdjust* Adjust() const { return active_ ? &adjust_ : nullptr; }
  // Called from the EndScene hook for the main frame.
  void OnEndScene(IDirect3DDevice9* dev);

  void Abort(const char* reason);
  float Progress() const;
  // Status line for the UI ("SAVED ..." / errors); empty if none. Clears it.
  std::string TakeMessage();
  bool Saving() const;

 private:
  void Release();  // surfaces and buffers
  void ResolveTile();
  void Finish();
  void Notify(const std::string& m);

  bool active_ = false;
  CaptureParams p_;
  u2::ProjAdjust adjust_;
  int width_ = 0, height_ = 0;
  int seq_ = 0, total_ = 0, warmup_ = 0;
  bool armed_ = false;      // this frame's EndScene should be grabbed
  IDirect3DSurface9* rt_ = nullptr;
  IDirect3DSurface9* sys_ = nullptr;
  std::vector<float> accum_;
  std::vector<uint8_t> image_;
  std::atomic<int> saving_{0};  // PNG writer threads still running

  mutable std::mutex msgMutex_;
  std::string message_;
};

}  // namespace photomode
