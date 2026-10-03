#pragma once
// Capture on top of the GPU accumulator (accum.h): per tile, N jittered samples (lens position,
// sub-pixel offset, optional shutter time slice) are folded into the running mean, then one
// resolve frame shows the mean with the FE hidden and its backbuffer is read back at EndScene
// (after WidescreenFix gamma). Tiles are stitched and a worker thread writes the PNG.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sampling.h"
#include "u2sdk/u2math.h"

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace photomode {

struct CaptureParams {
  SampleParams sample;      // tiles 1..lens::kMaxTiles
  float shutterSec = 0;     // 0 = no motion blur
  std::string outPath;      // full path of the .png
};

// What the controller does this frame.
struct CaptureFrame {
  bool sample = false;      // render sample `adj` and fold it in
  bool resolve = false;     // hide the FE, show the mean, read it back at EndScene
  bool newTile = false;     // reset the accumulator first (a new tile starts)
  u2::ProjAdjust adj;
  float nextGameSpeed = 0;  // shutter: game speed for the next simulation step (0 = frozen)
};

class Capture {
 public:
  ~Capture() { Release(); }

  // Allocates everything up front. On failure returns false and sets `error`.
  bool Start(IDirect3DDevice9* dev, const CaptureParams& p, std::string* error);
  bool Active() const { return active_; }
  const CaptureParams& Params() const { return p_; }

  // Called at the start of each rendered frame with the accumulator's sample count.
  CaptureFrame BeginFrame(int accumCount, float lastFrameDt);
  // Called from the EndScene hook when this frame's resolve image was drawn.
  void OnEndScene(IDirect3DDevice9* dev);

  void Abort(const char* reason);
  float Progress() const;
  // Status line for the UI ("SAVED ..." / errors); empty if none. Clears it.
  std::string TakeMessage();
  bool Saving() const;

 private:
  void Release();  // surfaces and buffers
  void Finish();
  void Notify(const std::string& m);

  bool active_ = false;
  CaptureParams p_;
  int width_ = 0, height_ = 0;
  int tile_ = 0, done_ = 0;       // current tile; samples done in it (for Progress)
  int lastPlanned_ = -1;          // sample index planned last frame (-1 none)
  bool resolveArmed_ = false;     // a resolve frame is pending its EndScene readback
  bool tileDone_ = false;         // the tile was read back; the next frame starts the next one
  IDirect3DSurface9* rt_ = nullptr;
  IDirect3DSurface9* sys_ = nullptr;
  std::vector<uint8_t> image_;
  std::atomic<int> saving_{0};  // PNG writer threads still running

  mutable std::mutex msgMutex_;
  std::string message_;
};

}  // namespace photomode
