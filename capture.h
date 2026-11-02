#pragma once
// Capture on top of the GPU accumulator (accum.h): per tile, N jittered samples (lens position,
// sub-pixel offset, optional shutter time slice) are folded into the running mean, then one
// resolve frame draws the mean and the photo's overlays into an off-screen target (never the screen, so in-game FSAA
// and WidescreenFix's edge clearing never reach the photo), runs WidescreenFix's gamma on it (wsfgamma.h) and reads it
// back. Tiles are stitched and a worker thread writes the PNG.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sampling.h"
#include "u2sdk/u2math.h"
#include "wsfgamma.h"

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;

namespace photomode {

struct CaptureParams {
  SampleParams sample;      // tiles 1..lens::kMaxTiles (image is tiles x tiles screens)
  bool supersample = false; // 2x: one pass into a 2W x 2H target (supersample.h), resolved in quarters
  float shutterSec = 0;     // 0 = no motion blur
  std::string outPath;      // full path of the .png
  int aspect = 0;           // Aspect Ratio (frame.h): the bars are drawn into the image
  bool crop = false;        // Crop to Ratio: the PNG is the photo area only
  // [Debug] Verbose, 1x only: one more frame shows the photo's view on screen with the FE hidden, read back after
  // WidescreenFix at EndScene as photos were until 2026-11-02, and compared with the photo (CheckScreen, logged).
  bool screenCheck = false;
};

// What the controller does this frame.
struct CaptureFrame {
  bool sample = false;      // render sample `adj` and fold it in
  bool resolve = false;     // draw the mean off screen and read it back (ResolveTarget, ReadResolved)
  bool newTile = false;     // reset the accumulator first (a new tile starts)
  bool warm = false;        // supersample: render at 2x without sampling (glow buffer warm-up)
  int region = -1;          // supersample resolve: quarter of the 2x mean to resolve (QuarterUv)
  bool check = false;       // the screen check frame (CaptureParams::screenCheck)
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
  // A resolve frame: the caller draws this screen piece (the mean and the photo's overlays) into ResolveTarget, a
  // screen-sized render target, then ReadResolved gammas it and copies it into the image.
  IDirect3DSurface9* ResolveTarget() const { return active_ && resolveArmed_ ? surf_ : nullptr; }
  void ReadResolved(IDirect3DDevice9* dev);
  // From EndScene on the check frame: the screen against the photo, logged; then the PNG.
  void CheckScreen(IDirect3DDevice9* dev);

  void Abort(const char* reason);
  float Progress() const;
  // Status line for the UI ("SAVED ..." / errors); empty if none. Clears it.
  std::string TakeMessage();
  bool Saving() const;
  int Tile() const { return tile_; }  // the tile or 2x quarter being sampled / resolved

 private:
  void Release();  // surfaces and buffers
  void Finish();
  void Notify(const std::string& m);

  bool active_ = false;
  CaptureParams p_;
  int width_ = 0, height_ = 0;
  int tile_ = 0, done_ = 0;       // current tile; samples done in it (for Progress)
  int lastPlanned_ = -1;          // sample index planned last frame (-1 none)
  int warm_ = 0;                   // supersample warm-up frames planned
  bool resolveArmed_ = false;     // a resolve is pending its readback
  bool tileDone_ = false;         // the tile was read back; the next frame starts the next one
  bool checkPending_ = false;     // the photo is read; the screen check frame comes next
  wsfgamma::Gamma gamma_;
  wsfgamma::Use gammaUse_ = wsfgamma::Use::None;
  IDirect3DTexture9* tex_ = nullptr;   // the resolve target (a texture: the gamma reads it)
  IDirect3DSurface9* surf_ = nullptr;  // its level 0
  IDirect3DSurface9* out_ = nullptr;   // the gamma's output (only with the gamma on)
  IDirect3DSurface9* sys_ = nullptr;
  IDirect3DSurface9* checkRt_ = nullptr;  // the screen check's copy of the backbuffer
  std::vector<uint8_t> image_;
  std::atomic<int> saving_{0};  // PNG writer threads still running

  mutable std::mutex msgMutex_;
  std::string message_;
};

}  // namespace photomode
