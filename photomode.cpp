// NFSU2 Photo Mode: controller (state machine, hooks, menu, camera, capture glue).
#include <windows.h>
#include <shlobj.h>
#include <d3d9.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "accum.h"
#include "capture.h"
#include "focusprobe.h"
#include "follow.h"
#include "frame.h"
#include "grid.h"
#include "lens.h"
#include "mute.h"
#include "pauseentry.h"
#include "rig.h"
#include "sampling.h"
#include "supersample.h"
#include "tags.h"
#include "watermark.h"
#include "watermark_res.h"
#include "settings.h"
#include "u2sdk/optmenu.h"
#include "u2sdk/crash.h"
#include "u2sdk/d3d.h"
#include "u2sdk/dev.h"
#include "u2sdk/feng.h"
#include "u2sdk/freeze.h"
#include "u2sdk/game.h"
#include "u2sdk/hook.h"
#include "u2sdk/input.h"
#include "u2sdk/log.h"
#include "u2sdk/mem.h"

namespace photomode {
namespace {
using namespace u2;

// ---- world freeze ---------------------------------------------------------------------------
// Through the game's own pause branch at 0x58132C (u2sdk/freeze.h, shared with the daylight tuner).
// (Tweak_GameSpeed = 0 is NOT equivalent: it still runs World::DoTimestep with dt = 0, which left the
// game permanently frozen after exiting.)

enum class State { Off, Active, Capturing };
// User 2026-10-19: Focus Mode got its own row (Enter on Focus cycling the modes was not discoverable); Focus is the
// distance, editable only in Manual. Quality left the menu to free the row: [Capture] DefaultQuality still sets it
// and its code stays (lens.h kQualitySamples).
// User 2026-10-19: Target is "Focus Target", above Focus Mode (it is also the orbit camera's car).
// User 2026-10-20: Drive (beta request), the last row: the menu scrolls to it (12 rows, 11 slots; u2sdk optmenu).
// User 2026-10-20: Watermark (default None) and Watermark Pos. (disabled while None), before Take Picture.
// User 2026-10-22: Take Picture is the last row, Drive above it (MenuRow skips Drive when [PhotoMode] Drive = 0).
// User 2026-10-31 (release): Wheel Angle, Rain Drops and Sound Effects, after Grid. Racer tags are key T only.
enum Row { RowCamera, RowFocal, RowRoll, RowTarget, RowFocusMode, RowFocus, RowAperture, RowShutter, RowFollow, RowResolution,
           RowAspect, RowCrop, RowGrid, RowWheel, RowRain, RowSound, RowWatermark, RowWatermarkPos, RowDrive, RowTake,
           RowCount };

constexpr float kMinFocusM = 0.3f, kMaxFocusM = 500;
constexpr float kZoomRepeatFirst = 0.35f, kZoomRepeat = 0.12f;  // pad X / Y held
constexpr const char* kFooterKeys = "H - Hide/Show UI    Space - Take Picture";
constexpr const char* kFooterPad = "B - Hide/Show UI    A - Take Picture";
constexpr float kHintSettleSec = 0.25f;
constexpr float kBackGuardSec = 0.3f;  // after a key cancels a capture, the same press must not also exit
constexpr float kMinFrameLimit = 1 / 1800.f;  // see addr::FrameLimit
// The pause-menu icon resumes the race first (pauseentry.cpp); photo mode opens once the game has unpaused, given up
// after this long. The game unpauses within a few frames (Resume Race's own action); 3 s also covers a slow frame.
constexpr float kPauseOpenWindowSec = 3;
// Closing back to the pause menu: the world stays frozen while the game's pause handler brings it back, given up after
// this long (then the race resumes, logged "did not come back").
constexpr float kPauseReturnWindowSec = 2;

struct Saved {
  float gameSpeed;
  int dofEnable, mbEnable, drawHud;
  uint16_t fov;
  uint8_t cursorVisible;
  int rainEnable;
};

// What the Render3D post-hook does this frame (decided in Tick, before the frame renders).
struct FramePlan {
  bool sample = false;  // fold this frame into the accumulator (projection jittered by adj)
  bool show = false;    // draw the accumulated image over the scene
  bool hold = false;    // tile capture start: keep the frame on screen, then empty the mean
  bool showHold = false;// tile capture: draw the held frame over the zoomed tile sample
  bool resolve = false; // capture resolve frame: the mean is drawn off screen and read back (ResolvePhoto)
  bool warm = false;    // 2x capture warm-up: render into the 2x targets, don't sample
  int region = -1;      // supersample resolve: quarter of the 2x mean to resolve
  bool probeHold = false;// focus probe, frame N: keep this frame (Accum::Hold) without emptying the mean
  bool probe = false;    // focus probe, frame N+1: render the depth (focusprobe.h), show the held frame
  ProjAdjust adj;
};

// One entry of the Watermark row: a built-in (no path) or an image in the watermarks folder.
struct WmEntry {
  std::string name;
  std::wstring path;
  uint64_t stamp = 0;     // the file's write time and size: the image is decoded again only when they change
  watermark::Mark mark;   // its image is null until chosen (folder images; EnsureWatermark)
  bool bad = false;       // it could not be decoded
};

// What the focus probe measured for: the camera, the lens and the Focus Target.
struct PoseKey {
  Vec3 eye, fwd, up;
  float focal = 0;
  uint8_t* car = nullptr;
  bool operator==(const PoseKey& o) const {
    auto same = [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    return same(eye, o.eye) && same(fwd, o.fwd) && same(up, o.up) && focal == o.focal && car == o.car;
  }
};

// Center / Closest = autofocus on the target car's centre / nearest corner. Aim = whatever the
// view's centre ray hits first (static world or any car).
enum class FocusMode { Center, Closest, Aim, Manual };

struct PhotoMode {
  Settings cfg;
  Input input;
  Rig rig;
  Capture capture;
  State state = State::Off;
  Saved saved{};

  // menu values
  float focalMm = 50;  // any whole mm between the table's ends (lens::StepFocal)
  int fstopIdx = lens::kDefaultFStop, shutterIdx = 0, qualityIdx = 1, tiles = 1;
  FocusMode focusMode = FocusMode::Closest;  // user 2026-10-19: default Closest
  float focusDist = 5;
  float aimDist = 10;           // last aim hit; kept while the ray hits nothing
  bool aimHit = false;          // the ray hit something this frame
  int aimLogged = -2;           // verbose: last logged aim source (-1 none, 0 world, 1 car)
  float aimLoggedDist = 0;
  Follow follow = Follow::Rig;  // how the camera follows the target car while the shutter is open
  CamPose captureCam{};         // camera and car when the shutter opened
  CarPose captureCar{};
  bool uiHidden = false;
  bool grid = false;            // Grid row (grid.h): thirds lines on screen, never in the photo
  bool racerTags = false;       // key T: stays as left for the session (DrawTags)
  bool rainDrops = true;        // Rain Drops row: the drops on the lens (g_RainEnable, ApplyRain)
  bool sound = false;           // World Sound row: off while photo mode is open (mute.h), back while driving
  // Wheel Angle row: only what is drawn (WheelScope), per car, forgotten on closing and on Drive.
  struct WheelSet { uint8_t* car; float deg; };
  std::vector<WheelSet> wheels;
  Mat4 viewProj{};              // view 1's, without the sample jitter (ProjDetour), for the tags
  bool viewProjValid = false;
  float particleCarry = 0;      // shutter exposures: particle time not stepped yet (ParticleStepDetour)
  float zoomRepeat = 0;         // pad X / Y held: seconds to the next focal length step
  probe::FocusProbe probe;      // precise Aim / Closest (focusprobe.h)
  int probeStage = 0;           // 1: frame held, the probe renders next frame
  PoseKey lastPose, probedPose;
  bool probed = false;          // the probe ran for probedPose
  bool probeOk = false;         // and read: probeRes is valid
  probe::Result probeRes;
  bool captureAfterProbe = false;  // Take Picture before the probe ran: the capture starts after it
  int aspect = 0;               // Aspect Ratio row (frame.h), 0 Native
  bool crop = true;             // Crop to Ratio: the PNG without the bars
  grid::Overlay quadOverlay;    // the Aspect Ratio bars and the Grid lines (grid.h)
  // Watermarks (watermark.h): 0 None, 1 NFS-Planet, 2 NFS (both inside the ASI), then the images in the "watermarks" folder.
  int watermark = 0;
  watermark::Corner wmCorner = watermark::Corner::BottomRight;
  std::vector<WmEntry> wm;      // the Watermark row's list (LoadWatermarks): None, the built-ins, the folder images
  watermark::Overlay wmOverlay;
  bool driving = false;         // Drive: the world runs and the car takes the driving keys (StartDrive)
  uint8_t savedShowcaseKeys = 0, savedShowcaseDpad = 0;
  int finishAtDrive = 0;        // the player's finish tick when driving started (PlayerFinishTick)
  bool openedBefore = false;    // Camera Mode, Focal Length and Roll are kept from the previous opening (Enter)
  // The camera when photo mode closed, relative to its car (SaveView / RestoreView).
  struct View {
    bool valid = false;
    Rig rig;
    uint8_t* ref = nullptr;     // the car it was relative to; null = the player
    uint8_t* target = nullptr;  // the Focus Target; null = the player
    Vec3 carPos{};
    float carHeading = 0;       // degrees
  } view;
  std::string toast;
  float toastTime = 0;
  float backGuard = 0;          // seconds left in which Back/close keys are ignored
  float hintSettle = 0;         // seconds after opening in which keys and the mouse do not count for the footer
  bool padHint = false;         // the last input came from the pad: the menu's footer shows its buttons
  bool stickHint = false;       // a stick or trigger was off centre last frame

  Vec3 anchor{};
  // Free mode: the car the camera's MaxDistance range stays around (the target when Free mode began), so a new
  // Focus Target only changes what Center / Closest focus on (user 2026-10-19).
  uint8_t* rangeCar = nullptr;
  uint8_t* target = nullptr;  // orbit target car (World car array entry); null = player
  LARGE_INTEGER lastQpc{}, qpcFreq{};
  float lastDt = 1 / 60.f;      // the previous open frame's time (the camera and shutter steps use it)
  alignas(16) bMatrix4 camMatrix{};  // world->view matrix applied in the SetCameraMatrix hook
  bool firstFrameDone = false;
  float postExitLog = 0;      // verbose: seconds left to log game state after exit
  float pauseRequest = 0;     // seconds left to open photo mode after its pause-menu icon resumed the race
  bool fromPause = false;     // opened from the pause-menu icon: closing it goes back to the pause menu
  float returnToPause = 0;    // seconds left to bring the pause menu back (the world stays frozen meanwhile)
  bool pauseAsked = false;    // the game's pause handler was called for this return
  POINT cursorAtEnter{};

  // GPU accumulation (live preview and capture)
  Accum accum{"accum"};
  // 2x photos: the 3D pass renders into a 2W x 2H target; its own accumulator at that size.
  Supersample super;
  Accum bigAccum{"accum2x"};
  // A capture other than supersampled 2x with in-game FSAA: sample frames render into `super` at screen size,
  // without MSAA (StartCapture).
  bool noMsaa = false;
  bool resetBeforeSample = false;  // a fresh 1x capture: empty the mean at its first sample (Render3DDetour)
  int savedStreaks = -1, savedFog = -1, savedRain = -1;  // -1 = not changed
  bool rezeroNativeFx = false;  // a device reset put the player's DOF/MB flags back; zero them again
  SampleKey accKey{};          // what the accumulated image shows
  float stillTime = 0;         // seconds the key has not changed
  FramePlan frame;
  bool uncapped = false;       // frame limiter lowered (SetUncapped)
  float savedFrameLimit = 0;
  int savedMirror = 0;
  LARGE_INTEGER previewStart{};
  bool captureContinued = false;  // the capture started from the preview's samples
  int captureFromSample = 0;
  LARGE_INTEGER captureStart{};
};
PhotoMode* g = nullptr;

// ---- original functions -------------------------------------------------------------------
using SetCameraMatrixFn = void(__fastcall*)(CameraParams*, void*, bMatrix4*, float);
using MoverUpdateFn = void(__fastcall*)(void*, void*);
using ProjFn = void(__fastcall*)(void*, void*, eView*, float, float, int);
using FeRenderFn = void(__fastcall*)(void*, void*);
using FrameFn = void(__cdecl*)();
SetCameraMatrixFn oSetCameraMatrix;
MoverUpdateFn oCubicMoverUpdate;
ProjFn oProj;
FeRenderFn oFeRender;
FrameFn oFrame;
FrameFn oRender3D;

// ---- helpers --------------------------------------------------------------------------------
float FovCal() { return g->cfg.fovCalibration > 0 ? g->cfg.fovCalibration : kDefaultVertFovScale; }
float Focal() { return g->focalMm; }
float FStop() { return lens::kFStops[g->fstopIdx]; }

void Toast(const std::string& s, float seconds = 3) {
  g->toast = s;
  g->toastTime = seconds;
  U2LOG("toast: %s", s.c_str());
}

// The whole path (OutputDir may name nested folders); an existing folder is fine.
void EnsureOutputDir() { SHCreateDirectoryExA(nullptr, g->cfg.outputDir.c_str(), nullptr); }

// Target cars: the player and active AI racers (u2::RacerCars). Index into that list, or -1.
int TargetIndex(const RacerCar* cars, int n) {
  for (int i = 0; i < n; ++i)
    if (cars[i].car == g->target) return i;
  return -1;
}

uint8_t* TargetCar() { return g->target ? g->target : PlayerCar(); }

bool UpdateAnchor() {
  // A racer that drops out (finished, parked) hands the orbit back to the player, except while the
  // shutter is open: the exposure stays on the car it started with (Car* live for the session).
  if (g->target && g->state != State::Capturing) {
    RacerCar cars[kMaxCars];
    if (TargetIndex(cars, RacerCars(cars, kMaxCars)) < 0) g->target = nullptr;
  }
  uint8_t* car = TargetCar();
  if (!car) return false;
  g->anchor = CarPosition(car);
  return true;
}

// User 2026-11-01: a racer's own name in a race, as the standings show it (DriverInfo Car+0x14, +0x90C char[16];
// 0x4C93B8..0x4C93F0); in free roam (race type [0x89E7B0] == 0, verified-in-binary.md) a number.
constexpr uintptr_t kRaceType = 0x89E7B0;
std::string RacerName(const RacerCar& c) {
  if (Var<int>(kRaceType) != 0)
    if (auto driver = *reinterpret_cast<uint8_t**>(c.car + 0x14)) {
      const char* name = reinterpret_cast<const char*>(driver + 0x90C);
      if (name[0]) return std::string(name, strnlen(name, 16));
    }
  char b[32];
  std::snprintf(b, sizeof b, "Racer %d", c.racer);
  return b;
}

std::string TargetName() {
  RacerCar cars[kMaxCars];
  int i = TargetIndex(cars, RacerCars(cars, kMaxCars));
  if (i < 0 || cars[i].racer == 0) return "You";
  return RacerName(cars[i]);
}

// In Orbit mode the camera circles the new target; in Free mode it stays where it is (its range keeps the car it was
// near, rangeCar) and the target is only what Center / Closest focus on.
void CycleTarget(int dir) {
  if (g->rig.mode == RigMode::Free && !g->rangeCar) g->rangeCar = TargetCar();
  RacerCar cars[kMaxCars];
  int n = RacerCars(cars, kMaxCars);
  if (n < 2) {
    Toast("No other racers");
    return;
  }
  int i = TargetIndex(cars, n);
  g->target = cars[Wrap((i < 0 ? 0 : i) + dir, n)].car;
  UpdateAnchor();
}

// Target car (orbit target, else the player) as a pose: Car+0x50 position, Car+0x90 orientation
// rows (row 0 = forward; traffic spawner 0x42C8E7, NFSU2_CameraToolkit dllmain.cpp:151). Rows that
// are not unit vectors (car not set up yet) fall back to the world axes.
CarPose PoseOf(uint8_t* car, const Vec3& fallback) {
  CarPose c{fallback, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  if (car) {
    Vec3 axes[3];
    CarAxes(car, axes);
    c.pos = CarPosition(car);
    if (std::fabs(Length(axes[0]) - 1) < 0.1f && std::fabs(Length(axes[2]) - 1) < 0.1f)
      for (int r = 0; r < 3; ++r) c.axis[r] = axes[r];
    CarLocalBox(car, &c.boxMin, &c.boxMax);  // the car's own body box; the fallback box stays without one
  }
  return c;
}
CarPose TargetCarPose() { return PoseOf(TargetCar(), g->anchor); }

CamPose RigPose() {
  Vec3 eye, target, up;
  g->rig.Compute(g->anchor, &eye, &target, &up);
  return {eye, Normalize(target - eye), up};
}

PoseKey CurrentPoseKey() {
  const CamPose c = RigPose();
  return {c.eye, c.fwd, c.up, Focal(), TargetCar()};
}
// The focus probe ran for the camera as it is now (read or not; a failed read is not repeated until it moves).
bool ProbedHere() { return g->probed && g->probedPose == CurrentPoseKey(); }
// Its result (focusprobe.h), or null while the camera moves, before it ran, or when its read failed: the ray and box
// methods then stay (review 2026-10-22: an empty result had overwritten a valid Aim ray hit with "no hit").
const probe::Result* FreshProbe() { return g->probeOk && ProbedHere() ? &g->probeRes : nullptr; }

// Autofocus: the car's centre, or its nearest point (photographers focus on the near headlight): the nearest drawn
// point once the focus probe has run, else the nearest corner of an approximate car box.
float AutoFocusDistance(FocusMode mode) {
  if (mode == FocusMode::Aim) return g->aimDist;
  CamPose cam = RigPose();
  CarPose car = TargetCarPose();
  float d = mode == FocusMode::Closest ? NearestCarDepth(cam, car) : CarCentreDepth(cam, car);
  if (mode == FocusMode::Closest)
    if (const probe::Result* r = FreshProbe(); r && r->carHit) d = r->carZ;
  return d < kMinFocusM ? kMinFocusM : d;
}
// Aim autofocus: nearest hit of the view's centre ray on the static world (u2::WorldRayHit) or a
// car box (every car in play, not only the target). With no hit the last distance is kept.
void UpdateAim() {
  CamPose cam = RigPose();
  Vec3 dir = Normalize(cam.fwd);
  float best = kMaxFocusM;
  int source = -1;
  if (float d; WorldRayHit(cam.eye, dir, kMaxFocusM, &d) && d < best) best = d, source = 0;
  uint8_t* cars[kMaxCars];
  for (int i = 0, n = ActiveCars(cars, kMaxCars); i < n; ++i) {
    Vec3 axes[3];
    CarAxes(cars[i], axes);
    CarPose pose{CarPosition(cars[i]), {axes[0], axes[1], axes[2]}};
    CarLocalBox(cars[i], &pose.boxMin, &pose.boxMax);
    float d = RayCarBoxHit(cam.eye, dir, pose, best);
    if (d >= 0 && d < best) best = d, source = 1;
  }
  g->aimHit = source >= 0;
  if (g->aimHit) g->aimDist = best < kMinFocusM ? kMinFocusM : best;
  if (const probe::Result* r = FreshProbe()) {  // what is drawn at the centre (focusprobe.h) replaces the ray
    g->aimHit = r->aimHit;
    if (r->aimHit) g->aimDist = r->aimZ < kMinFocusM ? kMinFocusM : r->aimZ;
    source = r->aimHit ? 2 : -1;
  }
  if (g->cfg.verbose && (source != g->aimLogged || std::fabs(g->aimDist - g->aimLoggedDist) > 0.1f * g->aimLoggedDist)) {
    static const char* names[] = {"none", "world", "car", "depth"};
    U2LOG("aim: %s %.2f m (eye %.1f %.1f %.1f dir %.2f %.2f %.2f)", names[source + 1], g->aimDist, cam.eye.x, cam.eye.y,
          cam.eye.z, dir.x, dir.y, dir.z);
    g->aimLogged = source;
    g->aimLoggedDist = g->aimDist;
  }
}

float FocusDistance() { return g->focusMode == FocusMode::Manual ? g->focusDist : AutoFocusDistance(g->focusMode); }

// ---- GPU accumulation --------------------------------------------------------------------
// While samples are accumulated with the world frozen, the game's frame limiter (busy wait in
// 0x57EAD0 on addr::FrameLimit, 1/60 with WidescreenFix FPSLimit=-1) only slows us down.
void SetUncapped(bool on) {
  if (on == g->uncapped) return;
  if (on) {
    g->savedFrameLimit = Var<float>(addr::FrameLimit);
    Var<float>(addr::FrameLimit) = kMinFrameLimit;
  } else {
    Var<float>(addr::FrameLimit) = g->savedFrameLimit;
  }
  g->uncapped = on;
}

SampleKey CurrentKey() {
  CamPose c = RigPose();
  SampleParams p{lens::kQualitySamples[g->qualityIdx], 1, lens::ApertureRadiusM(Focal(), FStop()), FocusDistance(),
                 g->cfg.antialias};
  return {c.eye, c.fwd, c.up, FocalToGameFov(Focal(), FovCal()), p, 0};
}

void StartCapture();

// The focus probe (focusprobe.h) for Aim / Closest with an aperture: once the camera has stood still for a frame,
// that frame is held (frame N) and the next one renders the depth instead (frame N+1). Then nothing until the
// camera, the lens or the Focus Target changes.
bool ProbeWanted() {
  return (g->focusMode == FocusMode::Aim || g->focusMode == FocusMode::Closest) && FStop() > 0 && !g->probe.Failed();
}
bool ProbePending() { return ProbeWanted() && !ProbedHere(); }

// Frame N+1 follows only once frame N was really held (Render3DDetour sets probeStage 1): on a frame the game skips
// Render3D (verified-in-binary.md "Frame pacing"), the probe frame would otherwise show an old or empty copy.
void PlanProbe() {
  const PoseKey k = CurrentPoseKey();
  const bool still = k == g->lastPose;
  g->lastPose = k;
  if (!ProbePending() || !still) {
    g->probeStage = 0;
    if (g->captureAfterProbe && !ProbePending()) {
      g->captureAfterProbe = false;
      StartCapture();
    }
    return;
  }
  if (g->probeStage == 1) {  // frame N+1: the probe replaces this frame's sample
    g->frame = {};
    g->frame.probe = true;
    g->probeStage = 0;
    SetUncapped(false);
    return;
  }
  if (!g->accum.Ensure(Device())) return;  // the held copy lives in the accumulator
  g->frame.probeHold = true;
}

void OnProbe(bool ok, const probe::Result& r) {
  g->probedPose = CurrentPoseKey();
  g->probed = true;
  g->probeOk = ok;
  g->probeRes = ok ? r : probe::Result{};
  if (g->cfg.verbose)
    U2LOG("focus probe: %s, aim %s %.2f m, car %s %.2f m (%d texels)", ok ? "read" : "FAILED",
          g->probeRes.aimHit ? "hit" : "none", g->probeRes.aimZ, g->probeRes.carHit ? "hit" : "none",
          g->probeRes.carZ, g->probeRes.carTexels);
  if (g->focusMode == FocusMode::Aim) UpdateAim();
}

// Live preview: once the camera and settings have been still for [Preview] Delay, accumulate
// jittered samples (the same ones a photo uses) up to the quality's sample count and show the mean.
void UpdatePreview(float dt) {
  SampleKey key = CurrentKey();
  if (key == g->accKey) {
    g->stillTime += dt;
  } else {
    g->accum.Reset();
    g->accKey = key;
    g->stillTime = 0;
  }
  // The accumulator (about 74 MB at 1080p) only once something can use it: the preview with an aperture or AA, or
  // a capture (StartCapture creates it). Opening photo mode allocates nothing more (2026-10-18 review).
  bool want = g->cfg.previewEnable && (key.p.lensRadius > 0 || key.p.antialias);
  bool ready = (want || g->accum.Created()) && g->accum.Ensure(Device());
  int n = g->accum.Count();
  bool sample = ready && g->cfg.previewEnable && g->stillTime >= g->cfg.previewDelay &&
                (key.p.lensRadius > 0 || key.p.antialias) && n < key.p.samples;
  g->frame.sample = sample;
  g->frame.show = ready && n > 0;
  g->frame.adj = sample ? SampleAdjust(key.p, n, 0, g->accum.Width(), g->accum.Height()) : ProjAdjust{};
  SetUncapped(sample);
  if (!g->cfg.verbose) return;
  if (sample && n == 0) QueryPerformanceCounter(&g->previewStart);
  if (!sample && n == key.p.samples && g->previewStart.QuadPart) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = 1000.0 * (now.QuadPart - g->previewStart.QuadPart) / g->qpcFreq.QuadPart;
    U2LOG("preview: %d samples in %.0f ms (%.0f/s)", n, ms, n * 1000.0 / (ms > 0 ? ms : 1));
    g->previewStart.QuadPart = 0;
  }
}

// Camera pose for this frame: the rig, or while a shutter exposure runs the shutter-follow pose.
CamPose CurrentPose() {
  if (g->state == State::Capturing && g->capture.Params().shutterSec > 0)
    return FollowPose(g->follow, g->captureCam, g->captureCar, TargetCarPose());
  return RigPose();
}

std::string NewPicturePath() {
  EnsureOutputDir();
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char name[64];
  std::strftime(name, sizeof(name), "NFSU2_%Y%m%d_%H%M%S", &tm);
  std::string base = g->cfg.outputDir + "\\" + name;
  std::string path = base + ".png";
  for (int i = 2; GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
    path = base + "_" + std::to_string(i) + ".png";
  return path;
}

// ---- camera ---------------------------------------------------------------------------------
// While photo mode is open the game's chase camera (CubicCameraMover::Update 0x453570) is frozen,
// like NFSU2 Extra Options' "Freeze Camera" hotkey (HotkeyStuff.h writes `ret` there). That also
// keeps WidescreenFix's mouse-look (which lives inside that update) from accumulating offsets.
// We then feed the player camera ourselves through Camera::SetCameraMatrix 0x447280, the same
// function NFSU2_CameraToolkit drives.
void ApplyCamera(float dt) {
  CameraParams* cam = PlayerCamera();
  CamPose p = CurrentPose();
  CreateLookAtMatrix(&g->camMatrix, p.eye, p.eye + p.fwd, p.up);
  cam->FieldOfView = FocalToGameFov(Focal(), FovCal());
  oSetCameraMatrix(cam, nullptr, &g->camMatrix, dt);
}

// ---- state transitions ----------------------------------------------------------------------
// The game never pauses in a network session (GameLoop 0x581306), so neither do we.
// Not while another ASI holds the freeze (the daylight tuner is open): its menu owns the options screen.
bool CanEnter() { return IsInGame() && !IsGamePaused() && !IsNetworkSession() && Device() && !freeze::AnyHeld(); }

// User 2026-10-20 (after the beta report on reset settings): the camera comes back where it was left, relative to its
// car (the Focus Target, or in Free mode the car its range stayed around), turned with the car's heading. Height,
// pitch and roll stay level with the world, so a car on a slope or on its roof does not tilt the view.
float CarHeading(uint8_t* car) {
  Vec3 axes[3];
  CarAxes(car, axes);  // row 0 = forward (TargetCarPose)
  return std::atan2(axes[0].y, axes[0].x) * kRadToDeg;
}

// The player or an active racer (u2::RacerCars includes the player). Pointers are only compared, never read.
bool IsActiveCar(uint8_t* car) {
  RacerCar cars[kMaxCars];
  for (int i = 0, n = RacerCars(cars, kMaxCars); i < n; ++i)
    if (cars[i].car == car) return true;
  return false;
}

void SaveView() {
  uint8_t* ref = g->rig.mode == RigMode::Free && g->rangeCar ? g->rangeCar : TargetCar();
  if (!ref) {
    g->view.valid = false;
    return;
  }
  g->view = {true, g->rig, ref == PlayerCar() ? nullptr : ref, g->target, CarPosition(ref), CarHeading(ref)};
}

// False when there is nothing to restore or its car has left the race: the camera then starts at the game camera.
bool RestoreView() {
  const auto& v = g->view;
  uint8_t* ref = v.ref ? v.ref : PlayerCar();
  if (!v.valid || !ref || !IsActiveCar(ref)) return false;
  g->target = v.target && IsActiveCar(v.target) ? v.target : nullptr;
  UpdateAnchor();
  const float turn = CarHeading(ref) - v.carHeading;
  const Vec3 pos = CarPosition(ref);
  g->rig = v.rig;
  g->rig.maxDist = g->cfg.maxDistance;
  g->rig.moveSpeed = g->cfg.moveSpeed;
  g->rig.yaw += turn;
  g->rig.eye = pos + RotateAxis(v.rig.eye - v.carPos, Vec3{0, 0, 1}, turn * kDegToRad);
  g->rangeCar = g->rig.mode == RigMode::Free && ref != TargetCar() ? ref : nullptr;
  return true;
}

// ---- watermarks ---------------------------------------------------------------------------------
// User 2026-10-20: each logo at its own pixel size on a 1080-high image (scaled with the image height); margins:
// NFS-Planet a little (10), NFS and the files some (24). The built-ins are decoded once; the images in the "watermarks"
// folder next to the ASI (watermark::ListName, sorted by name) are listed again each time photo mode opens, so they can
// be added or changed in game. A folder image is decoded only when it is chosen, and only the chosen one is kept; an
// unchanged file (same size and time) is not decoded again (review 2026-10-22: every image was decoded on every
// opening, about 14 ms each, up to 470 ms and 64 MB for a large one). The choice is kept by its name.
constexpr int kBuiltinMarks = 3;  // None, NFS-Planet, NFS

// The chosen mark's image, decoding a folder image on first use; false (logged once) when it can't be read.
bool EnsureWatermark(int i) {
  if (i <= 0 || i >= static_cast<int>(g->wm.size())) return false;
  WmEntry& e = g->wm[i];
  if (e.mark.image) return true;
  if (e.bad || e.path.empty()) return false;
  auto img = std::make_shared<watermark::Image>();
  if (!watermark::LoadFileImage(e.path, img.get())) {
    U2LOG("watermark: %ls not readable as an image (or larger than 4096 x 4096); left out", e.path.c_str());
    e.bad = true;
    return false;
  }
  e.mark = {img, 1, 24};
  U2LOG("watermark: \"%s\" from %ls %dx%d", e.name.c_str(), e.path.c_str(), img->w, img->h);
  return true;
}

// Choose mark i (0 = None): a folder image chosen before is released.
void ChooseWatermark(int i) {
  if (g->watermark != i && g->watermark >= kBuiltinMarks && g->watermark < static_cast<int>(g->wm.size()))
    g->wm[g->watermark].mark.image.reset();
  g->watermark = i;
}

void LoadWatermarks() {
  using namespace watermark;
  const std::string chosen = g->watermark < static_cast<int>(g->wm.size()) ? g->wm[g->watermark].name : "";
  if (g->wm.empty()) {
    auto planet = std::make_shared<Image>(), nfs = std::make_shared<Image>();
    g->wm.resize(kBuiltinMarks);
    g->wm[0].name = "None";
    g->wm[1].name = "NFS-Planet";
    g->wm[2].name = "NFS";
    if (LoadResourceImage(IDR_WM_NFSPLANET, planet.get())) g->wm[1].mark = {planet, 1, 10};
    if (LoadResourceImage(IDR_WM_NFS, nfs.get())) g->wm[2].mark = {nfs, 1, 24};
  }
  std::vector<WmEntry> old(g->wm.begin() + kBuiltinMarks, g->wm.end());
  g->wm.resize(kBuiltinMarks);
  std::vector<WmEntry> files;
  // The folder found by comparing names without case, so it is found even in a case-sensitive directory.
  std::wstring dir;
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((ModuleDir() + L"*").c_str(), &fd);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && _wcsicmp(fd.cFileName, L"watermarks") == 0) {
        dir = ModuleDir() + fd.cFileName + L"\\";
        break;
      }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  find = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((dir + L"*").c_str(), &fd);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      WmEntry e;
      constexpr DWORD kSkip = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;  // Thumbs.db
      if ((fd.dwFileAttributes & kSkip) || !ListName(fd.cFileName, &e.name)) continue;
      e.path = dir + fd.cFileName;
      e.stamp = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32 | fd.ftLastWriteTime.dwLowDateTime) ^
                (static_cast<uint64_t>(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow);
      for (const WmEntry& o : old)  // unchanged since the last listing: keep its image (or that it can't be read)
        if (o.path == e.path && o.stamp == e.stamp) e.mark = o.mark, e.bad = o.bad;
      files.push_back(std::move(e));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
  }
  // By name without case; the path decides between equal names ("logo.png", "logo.jpg"), so the order is stable.
  std::sort(files.begin(), files.end(), [](const WmEntry& a, const WmEntry& b) {
    const int c = _stricmp(a.name.c_str(), b.name.c_str());
    return c != 0 ? c < 0 : a.path < b.path;
  });
  for (WmEntry& e : files) g->wm.push_back(std::move(e));
  g->watermark = 0;
  for (int i = 1; i < static_cast<int>(g->wm.size()); ++i)
    if (g->wm[i].name == chosen && EnsureWatermark(i)) {
      g->watermark = i;
      break;
    }
  for (int i = kBuiltinMarks; i < static_cast<int>(g->wm.size()); ++i)  // only the chosen one stays decoded
    if (i != g->watermark) g->wm[i].mark.image.reset();
}

// Native DOF and motion blur off while photo mode is open (useless for photos, docs/kb/photomode-internals.md).
// DOF = 0 only when RenderFrame's DOF-off path is the game's own code (2026-10-22, a tester's crash with
// NFSU2HDReflections ImproveMotionBlur): with DOF 0, RenderFrame jumps to 0x5D29D1 (je at 0x5D298C), 2 bytes into the
// mod's 5-byte JMP at 0x5D29CF (AeroWidescreen NFSHDReflections SOURCE/NFSU2 dllmain.cpp:231-232), so its address bytes
// run as code; one outcome changes edi, which the reload test at 0x5D2BFA compares with [0x8707C4], and the game then
// reloads its shaders every frame (the crash in NFSU2ShaderLoader). Writing the saved values back is safe: only 0
// takes that branch. Motion blur's flag is not involved (the mod never reads it). verified-in-binary.md "Effect reload
// condition".
bool DofOffPathIsOriginal() {
  return std::memcmp(reinterpret_cast<const void*>(0x5D29CF), "\x7A\x0C\xC6\x05\xCC\x07\x87\x00\x00", 9) == 0;
}

void ZeroNativeFx(bool log) {
  Var<int>(addr::g_MotionBlurEnable) = 0;
  if (DofOffPathIsOriginal()) Var<int>(addr::g_DepthOfFieldEnable) = 0;
  else if (log) U2LOG("enter: native DOF left as it is: another mod patched its off path at 0x5D29CF (HD Reflections "
                      "ImproveMotionBlur?); zeroing it would crash");
}

// The drops on the lens (rain splatter 0x5C7AB0): g_RainEnable is read per frame only by its gate 0x5CB7E7, the other
// readers are the options and the render-target setup at a device reset (u2re xref), so falling rain stays. Only ever
// turned off, as for the tile capture (verified-in-binary.md "What changes on paused frames").
void ApplyRain() { Var<int>(addr::RainEnable) = g->rainDrops ? g->saved.rainEnable : 0; }

// Wheel Angle (user 2026-10-31: purely visual). The car pass draws the wheels from copies of the matrices Car+0x130[4]
// (spin + steer) and +0x230[4] (steer) that 0x5F6820 makes every frame (0x5F6B9B..0x5F6BD1, args of CarRenderInfo::Render
// read at 0x62710D); the sim builds those with 0x5921B0 (thiscall on Car+0x40, wheel 0 / 1; 0x59CBE6) from the u16
// Car+0x40E, 65536 = 360 degrees. (0x5EC7F0's render-time set is only a second draw: writing +0x40E alone did nothing,
// [game] 2026-11-01.) For the length of Render3D they are rebuilt at our angle and then put back, so the simulation
// never sees it, and each frame keeps the current spin and ride height for shutter blur.
constexpr uintptr_t kCarSteer = 0x40E, kCarWheelSpin = 0x130, kCarWheelSteer = 0x230, kCarSub = 0x40;
constexpr uintptr_t kBuildWheelMatrix = 0x5921B0;
constexpr size_t kFrontMatrices = 2 * 0x40;  // wheels 0 and 1, front only in 0x5921B0 (0x592303)
constexpr float kMaxWheelDeg = 45;
uint16_t& CarSteer(uint8_t* car) { return *reinterpret_cast<uint16_t*>(car + kCarSteer); }

class WheelScope {
 public:
  WheelScope() {
    if (g->state == State::Off) return;
    using BuildFn = void(__fastcall*)(void*, void*, int);
    const auto build = reinterpret_cast<BuildFn>(kBuildWheelMatrix);
    for (const auto& w : g->wheels) {
      if (n_ == kMaxCars) break;
      Saved& sv = saved_[n_++];
      sv.car = w.car;
      sv.steer = CarSteer(w.car);
      std::memcpy(sv.spin, w.car + kCarWheelSpin, kFrontMatrices);
      std::memcpy(sv.turn, w.car + kCarWheelSteer, kFrontMatrices);
      CarSteer(w.car) = static_cast<uint16_t>(static_cast<int>(std::lround(w.deg * 65536 / 360)) & 0xFFFF);
      for (int i = 0; i < 2; ++i) build(w.car + kCarSub, nullptr, i);
    }
  }
  ~WheelScope() {
    for (int i = 0; i < n_; ++i) {
      Saved& sv = saved_[i];
      CarSteer(sv.car) = sv.steer;
      std::memcpy(sv.car + kCarWheelSpin, sv.spin, kFrontMatrices);
      std::memcpy(sv.car + kCarWheelSteer, sv.turn, kFrontMatrices);
    }
  }

 private:
  struct Saved {
    uint8_t* car;
    uint16_t steer;
    uint8_t spin[kFrontMatrices], turn[kFrontMatrices];
  };
  Saved saved_[kMaxCars];
  int n_ = 0;
};

PhotoMode::WheelSet* FindWheel(uint8_t* car) {
  for (auto& w : g->wheels)
    if (w.car == car) return &w;
  return nullptr;
}

// A change the mean must not keep showing (the sample key does not cover it).
void RestartPreview() {
  g->accum.Reset();
  g->stillTime = 0;
}

// The first step starts from where the car's wheels are; Ctrl: tenths of a degree.
void StepWheel(int dir) {
  uint8_t* car = TargetCar();
  if (!car) return;
  auto* w = FindWheel(car);
  if (!w) {
    const float natural = static_cast<int16_t>(CarSteer(car)) * 360.0f / 65536;
    g->wheels.push_back({car, std::round(natural)});
    w = &g->wheels.back();
  }
  const float step = g->input.KeyDown(VK_CONTROL) ? 0.1f : 1.f;
  w->deg = Clamp(w->deg + dir * step, -kMaxWheelDeg, kMaxWheelDeg);
  RestartPreview();
}

// The sticks are the camera here, so they must not be the menu's directions too (user 2026-11-01). The FE id table 0x7F91C8
// (stride 0x1C: +0 joy event, +4 mask bit; fe-input.md 1.4) maps the events FENG_PAD_*_ALTERNATE 0x2E..0x31 to the same
// direction bits as FENG_PAD_* 0x2A..0x2D; the second set is the left stick (XtendedInput Config-UG2 ini binds them so:
// JOY_EVENT_FENG_PAD_UP_ALTERNATE = XINPUT_GAMEPAD_LS_UP, FENG_PAD_UP = DPAD_UP, VK_UP). With their mask bits 0 the
// GetJoyPadMask 0x50C6D0 never sets a direction from them. In memory, put back on closing.
//
// Pad A and B are the picture and the menu's hiding here, so they are not its select and cancel either. Entries 8..11
// are FENG_SELECT 0x25/0x26 (bit 0x10) and FENG_CANCEL 0x27/0x28 (bit 0x20); pad and keyboard share these events
// (XtendedInput: SELECT = GAMEPAD_A and VK_RETURN), so their bits are given back only while Enter or Esc is down
// (SyncFeSelectCancel): the FE's own path then does the rest, sound and scripts included.
constexpr uintptr_t kFeIdTable = 0x7F91C8, kFeIdStride = 0x1C;
constexpr int kFeFirstEntry = 4, kFeEntries = 8;
constexpr uint32_t kFeEntryIds[kFeEntries] = {0x2E, 0x2F, 0x30, 0x31, 0x25, 0x26, 0x27, 0x28};
uint32_t g_feMasks[kFeEntries];
bool g_feOff = false;
bool g_feKeyArmed[2];  // Enter, Esc seen up since opening: a key still held from before must not press a row
uintptr_t FeEntry(int i) { return kFeIdTable + (kFeFirstEntry + i) * kFeIdStride; }
void SetFePadInput(bool on) {
  if (on == !g_feOff) return;
  for (int i = 0; i < kFeEntries; ++i)
    if (Var<uint32_t>(FeEntry(i)) != kFeEntryIds[i]) {  // not the game's table: leave it
      U2LOG("FE id table entry %d is not the game's; the pad stays on the menu", i);
      return;
    }
  for (int i = 0; i < kFeEntries; ++i) {
    if (on) Var<uint32_t>(FeEntry(i) + 4) = g_feMasks[i];
    else {
      g_feMasks[i] = Var<uint32_t>(FeEntry(i) + 4);
      Var<uint32_t>(FeEntry(i) + 4) = 0;
    }
  }
  g_feKeyArmed[0] = g_feKeyArmed[1] = false;
  g_feOff = !on;
}

void SyncFeSelectCancel() {
  if (!g_feOff) return;
  const int keys[2] = {VK_RETURN, VK_ESCAPE};
  for (int k = 0; k < 2; ++k) {
    const bool down = g->input.KeyDown(keys[k]);
    if (!down) g_feKeyArmed[k] = true;
    for (int i = 4 + k * 2; i < 6 + k * 2; ++i)
      Var<uint32_t>(FeEntry(i) + 4) = down && g_feKeyArmed[k] ? g_feMasks[i] : 0;
  }
}

void Enter() {
  LoadWatermarks();
  g->rangeCar = nullptr;
  CameraParams* cam = PlayerCamera();
  g->saved = Saved{Var<float>(addr::GameSpeed), Var<int>(addr::g_DepthOfFieldEnable), Var<int>(addr::g_MotionBlurEnable),
                   Var<int>(addr::DrawHud), cam->FieldOfView, Var<uint8_t>(addr::FECursorVisible),
                   Var<int>(addr::RainEnable)};
  g->target = nullptr;
  if (g->cfg.verbose) LogCars();
  if (!UpdateAnchor()) g->anchor = ToVec3(cam->Position);
  g->rig.maxDist = g->cfg.maxDistance;
  g->rig.moveSpeed = g->cfg.moveSpeed;
  // Beta feedback 2026-10-20 ("the camera settings reset everytime I deactivate it"): Camera Mode, Focal Length,
  // Roll and the camera itself (RestoreView) stay as they were left (the other rows always did); only the first
  // opening takes the game camera's focal length. Without a view to restore the camera starts at the game's.
  const float roll = g->rig.roll;
  float focal = GameFovToFocal(cam->FieldOfView, FovCal());
  const bool restored = RestoreView();
  if (!restored) {
    g->rig.InitFromCamera(ToVec3(cam->Position), ToVec3(cam->Direction), g->anchor);
    g->rig.roll = roll;
  }
  if (!g->openedBefore) {
    g->focalMm = lens::kFocals[lens::Closest(lens::kFocals, focal)];
    g->openedBefore = true;
  }
  g->uiHidden = false;
  g->backGuard = 0;
  freeze::Hold(true);
  ZeroNativeFx(true);
  ApplyRain();
  mute::Set(!g->sound);
  SetFePadInput(false);
  g->hintSettle = kHintSettleSec;
  GetCursorPos(&g->cursorAtEnter);
  Var<uint8_t>(addr::FECursorVisible) = 0;  // hide the FE cursor, like the game's showcase photo shoot (0x4CEE11)
  g->input.SetMouseCapture(true);
  // Rear-view mirror off: it is drawn after Render3D, outside drawHud (0x5CC0D7), and would be in photos.
  g->savedMirror = Var<int>(addr::MirrorOverride);
  Var<int>(addr::MirrorOverride) = 1;
  g->accum.Reset();
  g->stillTime = 0;
  g->frame = {};
  g->state = State::Active;
  ApplyCamera(0);
  // Camera::StopUpdating (set only by mods such as ExOpts) makes SetCameraMatrix ignore us.
  U2LOG("enter: cam pos (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) fov %u (%.0fmm) car (%.1f %.1f %.1f) stopUpdating=%d view %s",
        cam->Position.x, cam->Position.y, cam->Position.z, cam->Direction.x, cam->Direction.y, cam->Direction.z,
        cam->FieldOfView, focal, g->anchor.x, g->anchor.y, g->anchor.z, Var<int>(addr::CameraStopUpdating),
        restored ? "restored" : "from the game camera");
}

// Screen-space effects that break at 2x (verified-in-binary.md "What changes on paused frames"):
// horizon fog sizes its grid from the screen. Supersampled 2x keeps light streaks/overbright (their
// buffers are swapped to 2x, supersample.cpp). Tiles also lose the streak/overbright pass (feedback
// ghosts between tiles) and the rain splatter (clip-space drops repeat per tile). Only ever turned
// off; restored by RestoreCaptureEffects.
void SetCaptureEffects(bool tiles) {
  g->savedFog = Var<int>(addr::HorizonFogEnable);
  Var<int>(addr::HorizonFogEnable) = 0;
  if (tiles) {
    g->savedStreaks = Var<int>(addr::LightStreaksEnable);
    g->savedRain = Var<int>(addr::RainEnable);
    Var<int>(addr::LightStreaksEnable) = 0;
    Var<int>(addr::RainEnable) = 0;
  }
}

void RestoreCaptureEffects() {
  if (g->savedStreaks >= 0) Var<int>(addr::LightStreaksEnable) = g->savedStreaks;
  if (g->savedFog >= 0) Var<int>(addr::HorizonFogEnable) = g->savedFog;
  if (g->savedRain >= 0) Var<int>(addr::RainEnable) = g->savedRain;
  g->savedStreaks = g->savedFog = g->savedRain = -1;
}

// SaveRegistrySettings 0x5BEEA0, cdecl(): writes every option global to the registry. The game calls it on every quit
// (the window procedure, 0x5CCE47, before PostQuitMessage), when leaving the options screen (0x49C89C) and from
// 0x4A82C0 / 0x4C5424 (investigation 2026-10-11, verified-in-binary.md "Option saves"). While photo mode is open
// DOF / motion blur (and during a capture fog, streaks, rain) are zeroed in memory, so a quit or a save then wrote
// the zeros as the player's choice ([game] 2026-10-11: "something keeps disabling motion blur and dof in settings").
// The save gets the player's values; ours go back right after. Entry 51 56 8B 35 08 30 78 00, or a leading JMP from
// NFSU2Daylight's hook on the same entry (MinHook chains).
constexpr uintptr_t kSaveRegistry = 0x5BEEA0;
using SaveRegFn = void(__cdecl*)();
SaveRegFn oSaveRegistry;
void __cdecl SaveRegistryDetour() {
  if (!g || g->state == State::Off) return oSaveRegistry();
  int dof = Var<int>(addr::g_DepthOfFieldEnable), mb = Var<int>(addr::g_MotionBlurEnable);
  int streaks = Var<int>(addr::LightStreaksEnable), fog = Var<int>(addr::HorizonFogEnable), rain = Var<int>(addr::RainEnable);
  Var<int>(addr::g_DepthOfFieldEnable) = g->saved.dofEnable;
  Var<int>(addr::g_MotionBlurEnable) = g->saved.mbEnable;
  if (g->savedStreaks >= 0) Var<int>(addr::LightStreaksEnable) = g->savedStreaks;
  if (g->savedFog >= 0) Var<int>(addr::HorizonFogEnable) = g->savedFog;
  Var<int>(addr::RainEnable) = g->saved.rainEnable;  // the Rain Drops row zeroes it too
  oSaveRegistry();
  Var<int>(addr::g_DepthOfFieldEnable) = dof, Var<int>(addr::g_MotionBlurEnable) = mb;
  Var<int>(addr::LightStreaksEnable) = streaks, Var<int>(addr::HorizonFogEnable) = fog, Var<int>(addr::RainEnable) = rain;
  U2LOG("registry save while photo mode is open: the player's DOF %d / motion blur %d were written", g->saved.dofEnable,
        g->saved.mbEnable);
}

// ---- drive -------------------------------------------------------------------------------------------
// User 2026-10-20 (beta request), like the career Showcase photo shoot's Control Car (research
// docs/research/2026-10-20-photomode-drive-and-scrolling.md): the world runs and the car drives while photo mode stays
// open; the camera stays ours (Orbit follows the car, Free stays put), the menu is hidden, the preview is off.
// - The freeze is released; the race coordinator stayed in state 4 under it, so the car is in its driving input mode.
// - The keyboard gets the game's map back (u2sdk optmenu SetGameInput, 0x5B7ED0 with the saved byte) and byte
//   0x836352 = 1, which the Showcase sets so the map selector 0x5CFBF0 picks its format 2: driving keys only, no
//   pause, camera, HUD, engage, map or SMS keys (template 0x7FD330). Its other readers (0x62118F, 0x630883, 0x630C29,
//   CarRenderInfo::Render) only draw racers at full detail and with their real trunk audio parts; wheels are not
//   affected (verified-in-binary.md "Showcase byte 0x836352"). Restored when driving stops.
// - The pause-key block 0x861E78 set by the menu stays, so pad Start does not pause either.
// - Format 2 also needs [0x836353] clear (u2::addr::ShowcaseDpad); it is saved and cleared as well (review 2026-10-22).

// The player's finish: in-world player 0 ([0x8900AC], PlayersByNumber) +0x50 is the world-frame tick at which it
// finished, 0 while racing (ctor 0x5F1670 clears it at 0x5F1808; 0x5EA530 sets it once from the race-end handlers,
// 0x5EA542..0x5EA54A; verified-in-binary.md "Pause menu icons"). User 2026-10-20: a race that finishes while driving
// in photo mode ends photo mode.
int PlayerFinishTick() {
  auto p = Var<uint8_t*>(addr::PlayersByNumber);
  return p ? *reinterpret_cast<int*>(p + 0x50) : 0;
}

void RestoreDriveKeys() {
  Var<uint8_t>(addr::ShowcaseKeys) = g->savedShowcaseKeys;
  Var<uint8_t>(addr::ShowcaseDpad) = g->savedShowcaseDpad;
}

void StartDrive() {
  if (g->driving || g->state != State::Active || !g->cfg.drive) return;
  g->driving = true;
  g->finishAtDrive = PlayerFinishTick();
  g->savedShowcaseKeys = Var<uint8_t>(addr::ShowcaseKeys);
  g->savedShowcaseDpad = Var<uint8_t>(addr::ShowcaseDpad);
  Var<uint8_t>(addr::ShowcaseKeys) = 1;
  Var<uint8_t>(addr::ShowcaseDpad) = 0;
  menu::SetHidden(true);
  menu::SetGameInput(true);
  g->wheels.clear();  // user 2026-10-31: Drive puts the wheels back
  mute::Set(false);
  freeze::Hold(false);
  g->accum.Reset();
  g->frame = {};
  SetUncapped(false);
  U2LOG("drive: on");
}

void StopDrive() {
  if (!g->driving) return;
  g->driving = false;
  RestoreDriveKeys();
  freeze::Hold(true);
  menu::SetGameInput(false);
  mute::Set(!g->sound);
  g->accum.Reset();
  g->stillTime = 0;
  // The pad button that stopped it must not also reach the menu once it shows again.
  g->backGuard = kBackGuardSec;
  U2LOG("drive: off");
}

void FreeSupersample() {
  g->noMsaa = false;
  g->resetBeforeSample = false;
  g->super.Free();
  g->bigAccum.Free();
}

// The focus probe and the overlays' GPU objects: on exit and before a device reset. A Take Picture still waiting for
// the probe stays pending after a reset (the probe runs again, then the capture starts) but not after exit.
void FreeProbeAndOverlays() {
  g->probe.Free();
  g->probed = g->probeOk = false;
  g->probeStage = 0;
  g->wmOverlay.Free();
  g->quadOverlay.Free();
}

// Closed by the player (not by the game pausing or leaving gameplay) after opening from the pause menu: back there.
void Exit(const char* why, bool byPlayer = false) {
  if (g->state == State::Off) return;
  const bool backToPause = byPlayer && g->fromPause && IsInGame();
  g->fromPause = false;
  if (g->driving) {  // the freeze is set below either way
    g->driving = false;
    RestoreDriveKeys();
  }
  // Only in gameplay: after leaving it the cars may be gone (RestoreView then starts from the game camera).
  if (IsInGame()) SaveView();
  else g->view.valid = false;
  g->capture.Abort("photo mode closed");
  g->frame = {};
  g->accum.Free();  // the targets are recreated on the next enter
  FreeProbeAndOverlays();
  g->captureAfterProbe = false;
  RestoreCaptureEffects();
  FreeSupersample();
  SetUncapped(false);
  g->wheels.clear();
  mute::Set(false);
  SetFePadInput(true);
  Var<int>(addr::RainEnable) = g->saved.rainEnable;
  g->viewProjValid = false;
  Var<int>(addr::MirrorOverride) = g->savedMirror;
  // Going back to the pause menu: the world stays frozen until the game's own pause has taken over (Tick).
  freeze::Hold(backToPause);
  if (backToPause) {
    g->returnToPause = kPauseReturnWindowSec;
    g->pauseAsked = false;
  }
  Var<float>(addr::GameSpeed) = g->saved.gameSpeed;
  Var<int>(addr::g_DepthOfFieldEnable) = g->saved.dofEnable;
  Var<int>(addr::g_MotionBlurEnable) = g->saved.mbEnable;
  Var<int>(addr::DrawHud) = g->saved.drawHud;
  PlayerCamera()->FieldOfView = g->saved.fov;
  tags::Close();
  menu::Close();
  g->input.SetMouseCapture(false);
  Var<uint8_t>(addr::FECursorVisible) = g->saved.cursorVisible;
  // Put the cursor back: the game/WidescreenFix mouse-look reads cursor deltas.
  SetCursorPos(g->cursorAtEnter.x, g->cursorAtEnter.y);
  g->state = State::Off;
  if (g->cfg.verbose) g->postExitLog = 3;
  U2LOG("exit (%s): speed=%.2f hud=%d", why, Var<float>(addr::GameSpeed), Var<int>(addr::DrawHud));
}

void StartCapture() {
  if (g->driving) return;  // stop driving first (the world is moving)
  if (g->state == State::Active && ProbePending()) {  // focus on the measured point: start right after the probe
    g->captureAfterProbe = true;
    return;
  }
  if (g->capture.Saving()) {
    Toast("Still saving the previous picture");
    return;
  }
  if (!g->accum.Ensure(Device())) {
    Toast("Capture failed: no GPU accumulation");
    return;
  }
  CaptureParams p;
  p.sample = {lens::kQualitySamples[g->qualityIdx], g->tiles, lens::ApertureRadiusM(Focal(), FStop()), FocusDistance(),
              g->cfg.antialias};
  p.shutterSec = lens::kShutters[g->shutterIdx];
  p.outPath = NewPicturePath();
  p.aspect = g->aspect;
  p.crop = g->aspect && g->crop;
  if (p.sample.tiles > 1) {
    // 2x: supersampled in one pass (motion blur works); tiles only when the targets don't fit. The probe's targets and
    // the held copies are not needed meanwhile (7-10 bytes a pixel; rebuilt on demand).
    g->probe.Free();
    g->accum.FreeHold();
    const int w = 2 * g->accum.Width(), h = 2 * g->accum.Height();
    p.supersample = g->super.Create(Device(), w, h) && g->bigAccum.Ensure(Device(), w, h);
    if (!p.supersample) {
      FreeSupersample();
      if (p.shutterSec > 0) {
        Toast("Capture failed: not enough video memory for 2x motion blur");
        return;
      }
    }
  }
  D3DSURFACE_DESC bb;
  if (!p.supersample && d3d::BackBufferDesc(Device(), &bb) && bb.MultiSampleType != D3DMULTISAMPLE_NONE) {
    // User 2026-10-21 ("don't like" the top line of 1x photos): with MSAA the game's full-screen post-FX quads (clip
    // +-1, verified-in-binary.md "Full-screen post-FX quads") leave the top/left half of row 0 and column 0 unwritten,
    // so those samples keep the image from before bleach. Rendered without MSAA, as 2x photos are, every pixel is
    // covered (FSAA-off and 2x photos measured clean). The jittered samples still smooth the edges. Tiles too.
    g->noMsaa = g->super.Create(Device(), g->accum.Width(), g->accum.Height());
    if (!g->noMsaa) {
      FreeSupersample();
      U2LOG("capture: no screen-size target; photo with in-game FSAA (top row / left column)");
    }
  }
  std::string err;
  if (!g->capture.Start(Device(), p, &err)) {
    FreeSupersample();
    Toast(err);
    return;
  }
  if (p.sample.tiles > 1) SetCaptureEffects(!p.supersample);
  if (p.supersample) g->bigAccum.Reset();
  // A 1x photo without shutter is exactly what the preview accumulates: continue from its samples
  // (instant when it has converged). Anything else starts from an empty mean. Not with in-game FSAA (noMsaa): the
  // preview's samples are MSAA frames with the light top row / left column (user 2026-10-21: fresh samples instead).
  g->captureContinued = p.sample.tiles == 1 && p.shutterSec == 0 && !g->noMsaa && CurrentKey() == g->accKey;
  // Tiles empty the mean in the Render3D hook, after holding the frame on screen (see FramePlan); a fresh 1x photo
  // empties it at its first sample (Render3DDetour), so the preview stays on screen until then.
  g->resetBeforeSample = !g->captureContinued && p.sample.tiles == 1;
  g->captureFromSample = g->captureContinued ? g->accum.Count() : 0;
  QueryPerformanceCounter(&g->captureStart);
  // Started from the controls (Space / pad X), nothing else plans this frame: keep showing the mean
  // so a converged preview doesn't flash the sharp scene for a frame.
  g->frame = {};
  g->frame.show = true;
  g->frame.hold = p.sample.tiles > 1 && !p.supersample;
  g->captureCam = RigPose();
  g->captureCar = TargetCarPose();
  g->particleCarry = 0;
  g->state = State::Capturing;
}

// Every way a capture ends (done, cancelled, failed, device reset) comes through here from Tick;
// Exit restores the same things itself.
void EndCapture() {
  // After a shutter exposure the camera stays where it ended instead of snapping back.
  if (g->capture.Params().shutterSec > 0) {
    CamPose end = CurrentPose();
    g->rig.SetPose(end.eye, end.fwd, end.up, g->anchor);
  }
  freeze::Hold(true);
  Var<float>(addr::GameSpeed) = g->saved.gameSpeed;
  Var<int>(addr::DrawHud) = g->saved.drawHud;
  RestoreCaptureEffects();
  FreeSupersample();
  SetUncapped(false);
  g->frame = {};
  // Only a continued capture leaves a mean that still matches the preview's key.
  if (!g->captureContinued) g->accum.Reset();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  U2LOG("capture end: %.0f ms (continued from sample %d: %s)",
        1000.0 * (now.QuadPart - g->captureStart.QuadPart) / g->qpcFreq.QuadPart, g->captureFromSample,
        g->captureContinued ? "yes" : "no");
  g->state = State::Active;
}

// ---- menu -----------------------------------------------------------------------------------
// Menu rows are read-only while the shutter is open: the exposure uses the settings it started with.
void Adjust(int row, int dir) {
  if (g->state != State::Active) return;
  switch (row) {
    case RowTarget: CycleTarget(dir); break;
    case RowCamera:
      g->rig.SetMode(g->rig.mode == RigMode::Orbit ? RigMode::Free : RigMode::Orbit, g->anchor);
      g->rangeCar = nullptr;  // a new Free mode starts around the target; Orbit circles the target
      break;
    // Ctrl: fine steps (user 2026-10-20), for the menu's Left / Right and the wheel alike.
    case RowFocal: g->focalMm = lens::StepFocal(g->focalMm, dir, g->input.KeyDown(VK_CONTROL)); break;
    case RowRoll: g->rig.roll = Clamp(std::round(g->rig.roll) + dir, -kMaxRoll, kMaxRoll); break;
    case RowFocusMode: {
      FocusMode prev = g->focusMode;
      g->focusMode = static_cast<FocusMode>(Wrap(static_cast<int>(g->focusMode) + dir, 4));
      if (g->focusMode == FocusMode::Aim) UpdateAim();
      // Manual starts at the distance the previous mode had, so the picture does not jump.
      if (g->focusMode == FocusMode::Manual) g->focusDist = Clamp(AutoFocusDistance(prev), kMinFocusM, kMaxFocusM);
      break;
    }
    case RowFocus:  // Manual only (IsRowEnabled)
      if (g->focusMode != FocusMode::Manual) break;
      {  // Ctrl: fine steps of 1 % (user 2026-10-21), as Ctrl on Focal Length
        const float k = g->input.KeyDown(VK_CONTROL) ? 1.01f : 1.08f;
        g->focusDist = Clamp(g->focusDist * (dir > 0 ? k : 1 / k), kMinFocusM, kMaxFocusM);
      }
      break;
    case RowAperture: g->fstopIdx = ClampInt(g->fstopIdx + dir, 0, lens::Count(lens::kFStops) - 1); break;
    case RowShutter: g->shutterIdx = ClampInt(g->shutterIdx + dir, 0, lens::Count(lens::kShutters) - 1); break;
    case RowFollow: g->follow = static_cast<Follow>(Wrap(static_cast<int>(g->follow) + dir, static_cast<int>(Follow::Count))); break;
    case RowResolution: g->tiles = ClampInt(g->tiles + dir, 1, lens::kMaxTiles); break;
    case RowAspect: g->aspect = Wrap(g->aspect + dir, frame::kCount); break;
    case RowCrop: if (g->aspect) g->crop = !g->crop; break;  // only with a ratio (IsRowEnabled)
    case RowGrid: g->grid = !g->grid; break;
    case RowWheel: StepWheel(dir); break;
    case RowRain:
      if (g->saved.rainEnable) {  // with the game's own option off there are no drops to turn off (IsRowEnabled)
        g->rainDrops = !g->rainDrops;
        ApplyRain();
        RestartPreview();
      }
      break;
    case RowSound:
      g->sound = !g->sound;
      mute::Set(!g->sound);
      break;
    case RowWatermark:
      // The next one that can be shown (None always can); an unreadable image is skipped from then on.
      for (int n = static_cast<int>(g->wm.size()), i = Wrap(g->watermark + dir, n); ; i = Wrap(i + dir, n))
        if (i == 0 || EnsureWatermark(i)) {
          ChooseWatermark(i);
          break;
        }
      break;
    case RowWatermarkPos:  // Pos. only matters with a watermark (IsRowEnabled)
      if (g->watermark) {
        constexpr int kCorners = static_cast<int>(watermark::Corner::Count);
        g->wmCorner = static_cast<watermark::Corner>(Wrap(static_cast<int>(g->wmCorner) + dir, kCorners));
      }
      break;
    default: break;
  }
}

bool IsActionRow(int row) { return row == RowTake || row == RowDrive; }
// The focus distance is shown live in every mode but only editable in Manual (native disabled row: grey, skipped).
bool IsRowEnabled(int row) {
  if (row == RowFocus) return g->focusMode == FocusMode::Manual;
  if (row == RowWatermarkPos) return g->watermark != 0;
  if (row == RowCrop) return g->aspect != 0;
  if (row == RowRain) return g->saved.rainEnable != 0;
  return true;
}

void Activate(int row) {
  if (g->state != State::Active) return;
  if (row == RowTake) StartCapture();
  else if (row == RowDrive) StartDrive();
  else if (row == RowWheel) {  // Enter: back to the car's own angle
    if (auto* w = FindWheel(TargetCar())) {
      g->wheels.erase(g->wheels.begin() + (w - g->wheels.data()));
      RestartPreview();
    }
  } else Adjust(row, 1);
}

// Row texts for the native menu (u2sdk optmenu draws them into UI_PauseOptions.fng).
std::string RowLabel(int row) {
  static const char* names[RowCount] = {"Camera Mode", "Focal Length", "Roll",     "Focus Target",
                                        "Focus Mode", "Focus",        "Aperture", "Shutter Speed",
                                        "Shutter Follow", "Resolution", "Aspect Ratio", "Crop to Ratio", "Grid", "Wheel Angle", "Rain Drops", "World Sound",
                                        "Watermark", "Watermark Pos.",
                                        "Drive", "Take Picture"};
  return row >= 0 && row < RowCount ? names[row] : "";
}

std::string RowValue(int row) {
  char b[64] = "";
  switch (row) {
    case RowCamera: return g->rig.mode == RigMode::Orbit ? "Orbit" : "Free";
    case RowTarget: return TargetName();
    case RowFocal: std::snprintf(b, sizeof b, "%.0f mm", Focal()); break;
    case RowRoll: std::snprintf(b, sizeof b, "%+.0f", g->rig.roll); break;
    case RowFocusMode: {
      static const char* modes[4] = {"Center", "Closest", "Aim", "Manual"};
      return modes[static_cast<int>(g->focusMode)];
    }
    case RowFocus:  // the distance the photo uses, live; Aim with nothing at the centre keeps its last distance
      if (g->focusMode == FocusMode::Aim && !g->aimHit) std::snprintf(b, sizeof b, "-- (%.1f m)", g->aimDist);
      else {  // centimetres under 10 m, so each fine step shows (user 2026-10-21)
        const float d = FocusDistance();
        std::snprintf(b, sizeof b, d < 10 ? "%.2f m" : "%.1f m", d);
      }
      break;
    case RowAperture: lens::FormatFStop(b, sizeof b, FStop()); break;
    case RowShutter: return lens::kShutterNames[g->shutterIdx];
    case RowFollow: return FollowName(g->follow);
    case RowResolution: std::snprintf(b, sizeof b, "%dx", g->tiles); break;
    case RowAspect: return frame::Name(g->aspect);
    case RowCrop: return g->crop ? "Yes" : "No";
    case RowGrid: return g->grid ? "On" : "Off";
    case RowWheel:
      if (const auto* w = FindWheel(TargetCar())) std::snprintf(b, sizeof b, "%+.1f", w->deg);
      else return "Natural";
      break;
    case RowRain: return g->rainDrops && g->saved.rainEnable ? "On" : "Off";
    case RowSound: return g->sound ? "On" : "Off";
    case RowWatermark: return g->watermark < static_cast<int>(g->wm.size()) ? g->wm[g->watermark].name : "None";
    case RowWatermarkPos: return watermark::CornerName(g->wmCorner);
    default: break;
  }
  return b;
}

// The menu's row index to ours: without Drive the menu has one row less and Take Picture moves up into Drive's place.
int MenuRow(int row) { return !g->cfg.drive && row >= RowDrive ? row + 1 : row; }
std::string MenuLabel(int row) { return RowLabel(MenuRow(row)); }
std::string MenuValue(int row) { return RowValue(MenuRow(row)); }
void MenuAdjust(int row, int dir) { Adjust(MenuRow(row), dir); }
void MenuActivate(int row) { Activate(MenuRow(row)); }
bool MenuIsAction(int row) { return IsActionRow(MenuRow(row)); }
bool MenuIsEnabled(int row) { return IsRowEnabled(MenuRow(row)); }

std::string StatusText() {
  char b[64];
  if (g->state == State::Capturing) {
    std::snprintf(b, sizeof b, "Capturing %d%%", static_cast<int>(g->capture.Progress() * 100));
    return b;
  }
  return g->toastTime > 0 ? g->toast : std::string();
}

void MenuBack() {
  if (g->state == State::Capturing) g->capture.Abort("cancelled");
  else if (g->backGuard <= 0) Exit("menu back", true);
}

// ---- per-frame ------------------------------------------------------------------------------
// Look: the mouse plus a stick (stickX / stickY, degrees per second at full tilt times dt). Narrow lenses get slower
// look so framing stays controllable.
void Look(RigInput* ri, float stickX, float stickY) {
  Input& in = g->input;
  const float zoomScale = Clamp(35.f / Focal(), 0.15f, 1.5f);
  const float ySign = g->cfg.invertY ? -1.f : 1.f;
  ri->lookYawDeg = (-in.MouseDX() * g->cfg.mouseSensitivity - stickX) * zoomScale;
  ri->lookPitchDeg = (-in.MouseDY() * g->cfg.mouseSensitivity + stickY) * zoomScale * ySign;
}

// Free mode keeps its range around rangeCar once the target changed (CycleTarget); else around the target.
void UpdateRig(const RigInput& ri, float dt) {
  const Vec3 range = g->rig.mode == RigMode::Free && g->rangeCar ? CarPosition(g->rangeCar) : g->anchor;
  g->rig.Update(ri, dt, range);
}

// While driving the keys and the pad drive the car: only the photo-mode key or pad Start (close), the Drive key or Esc /
// pad Back (stop driving) and mouse look are ours.
void HandleDriveControls() {
  Input& in = g->input;
  if (in.KeyPressed(g->cfg.hotkey) || in.PadPressed(PadStart)) {
    Exit("user", true);
    return;
  }
  if (in.KeyPressed(g->cfg.driveKey) || in.KeyPressed(VK_ESCAPE) || in.PadPressed(PadBack)) {
    StopDrive();
    return;
  }
  RigInput ri;
  Look(&ri, 0, 0);
  UpdateRig(ri, 0);
}

void HandleControls(float dt) {
  Input& in = g->input;
  const PadState& pad = in.Pad();
  if (g->driving) return HandleDriveControls();

  // Pad layout from a pad player (beta feedback 2026-10-31): A picture, B hide, Y / X lens in / out, the D-pad the menu;
  // The menu takes select and cancel from the keyboard only (SyncFeSelectCancel). Start closes, Back drives (user
  // 2026-11-02, swapped); Start does not also pause: the block byte stays set for 3 frames after closing (optmenu Close).
  if (g->backGuard <= 0 && (in.KeyPressed(g->cfg.hotkey) || in.PadPressed(PadStart))) {
    Exit("user", true);
    return;
  }
  if (in.KeyPressed('H') || in.PadPressed(PadB)) g->uiHidden = !g->uiHidden;
  if (in.KeyPressed('T')) g->racerTags = !g->racerTags;
  if (g->cfg.drive && (in.KeyPressed(g->cfg.driveKey) || in.PadPressed(PadBack))) {
    StartDrive();
    return;
  }
  if (in.KeyPressed(VK_SPACE) || in.PadPressed(PadA)) {
    StartCapture();
    return;
  }
  // Menu navigation (arrows / Enter / Esc, D-pad) is handled by the game's own FE.
  if (int w = in.Wheel()) Adjust(RowFocal, w);
  // One lens per press, then repeating while held.
  const int zoom = (in.PadDown(PadY) ? 1 : 0) - (in.PadDown(PadX) ? 1 : 0);
  if (in.PadPressed(PadY) || in.PadPressed(PadX)) {
    Adjust(RowFocal, zoom);
    g->zoomRepeat = kZoomRepeatFirst;
  } else if (zoom && (g->zoomRepeat -= dt) <= 0) {
    Adjust(RowFocal, zoom);
    g->zoomRepeat = kZoomRepeat;
  }
  if (in.KeyPressed(VK_OEM_4)) CycleTarget(-1);  // [ ] (pad: the Target row)
  if (in.KeyPressed(VK_OEM_6)) CycleTarget(1);

  RigInput ri;
  auto key = [&](int vk) { return in.KeyDown(vk) ? 1.f : 0.f; };
  auto button = [&](uint16_t b) { return in.PadDown(b) ? 1.f : 0.f; };
  ri.moveForward = key('W') - key('S') + pad.ly;
  ri.moveRight = key('D') - key('A') + pad.lx;
  ri.moveUp = key('E') - key('Q') + pad.rt - pad.lt;
  ri.speedMul = (in.KeyDown(VK_SHIFT) || in.PadDown(PadLS)) ? 4.f : ((in.KeyDown(VK_CONTROL) || in.PadDown(PadRS)) ? 0.25f : 1.f);
  Look(&ri, pad.rx * g->cfg.stickLookSpeed * dt, pad.ry * g->cfg.stickLookSpeed * dt);
  ri.rollDeg = (key('C') - key('Z') + button(PadRB) - button(PadLB)) * g->cfg.rollSpeed * dt;
  UpdateRig(ri, dt);
}

void LogPostExit(float dt) {
  float before = g->postExitLog;
  g->postExitLog -= dt;
  if (static_cast<int>(before) == static_cast<int>(g->postExitLog) && g->postExitLog > 0) return;
  auto rc = Var<int*>(addr::pRaceCoordinator);
  U2LOG("post-exit: flow=%d speed=%.2f hud=%d helpBar=%d rc=%d stop=%d", GameFlowState(), Var<float>(addr::GameSpeed),
        Var<int>(addr::DrawHud), Var<uint8_t>(addr::PCHelpBarUp), rc ? *rc : -1, Var<int>(addr::CameraStopUpdating));
}

void UpdateTags();

// The footer follows the last input (user 2026-11-02). Key, mouse button, wheel, a real mouse move, or a pad button or a
// stick / trigger leaving centre; a stick held over a key press does not take the hint back. While closed only the
// open key and the pad are read: the pad's A on the pause menu's icon counts. Just after opening keys and the mouse do
// not count: the first full read shows every held key as pressed, and the cursor has just been parked.
void UpdateFooterHint() {
  constexpr float kStickHint = 0.3f;
  constexpr float kMouseHint = 8;  // pixels in a frame: a nudged desk does not count
  const Input& in = g->input;
  const PadState& p = in.Pad();
  const bool stick = std::fabs(p.lx) > kStickHint || std::fabs(p.ly) > kStickHint || std::fabs(p.rx) > kStickHint ||
                     std::fabs(p.ry) > kStickHint || p.lt > kStickHint || p.rt > kStickHint;
  const bool pad = in.PadAnyPressed() || (stick && !g->stickHint);
  g->stickHint = stick;
  const int key = in.FirstKeyPressed();
  const float mouse = std::fabs(in.MouseDX()) + std::fabs(in.MouseDY());
  const bool keys = g->hintSettle <= 0 && (key != 0 || in.Wheel() != 0 || mouse > kMouseHint);
  const bool hint = keys ? false : pad ? true : g->padHint;
  if (hint == g->padHint) return;
  g->padHint = hint;
  U2LOG("footer hint: %s (key %d wheel %d mouse %.0f)", hint ? "pad" : "keyboard", key, in.Wheel(), mouse);
  menu::SetFooter(hint ? kFooterPad : kFooterKeys);
}

void Tick() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  float dt = g->lastQpc.QuadPart ? static_cast<float>(now.QuadPart - g->lastQpc.QuadPart) / g->qpcFreq.QuadPart : 1 / 60.f;
  g->lastQpc = now;
  dt = Clamp(dt, 0.0f, 0.1f);

  if (g->rezeroNativeFx) {  // after a device reset (OnDeviceReset); the reset frame itself draws nothing
    g->rezeroNativeFx = false;
    if (g->state != State::Off) {
      ZeroNativeFx(false);
      ApplyRain();
    }
  }
  menu::Tick();
  if (g->state == State::Off) {
    // Closed: only the key that can open it, the pad only for the L3 + R3 combo; the full update polls all 255 keys
    // and the mouse every frame (2026-10-18 review).
    const int keys[1] = {g->cfg.hotkey};
    g->input.UpdateOnly(keys, 1, g->cfg.padCombo);
  } else {
    g->input.Update();
  }
  UpdateFooterHint();
  SyncFeSelectCancel();
  if (g->hintSettle > 0) g->hintSettle -= dt;
  if (g->toastTime > 0) g->toastTime -= dt;
  if (g->backGuard > 0) g->backGuard -= dt;
  if (std::string m = g->capture.TakeMessage(); !m.empty()) Toast(m, 4);

  if (g->postExitLog > 0) LogPostExit(dt);
  if (g->state == State::Off) {
    if (g->returnToPause > 0) {
      g->returnToPause -= dt;
      if (IsPauseMenuState()) {
        g->returnToPause = 0;
        freeze::Hold(false);
      } else if (!g->pauseAsked) {
        g->pauseAsked = IsInGame() && pause::ReopenPauseMenu();
      }
      if (g->returnToPause <= 0 && !IsPauseMenuState()) {
        g->returnToPause = 0;
        freeze::Hold(false);
        U2LOG("pause menu: did not come back (asked %d, flow=%d)", g->pauseAsked, GameFlowState());
      }
    }
    // The pause-menu icon resumes the race as Resume Race does (pauseentry.cpp); open once the game has unpaused.
    if (pause::TakeRequest()) g->pauseRequest = kPauseOpenWindowSec;
    if (g->pauseRequest > 0) {
      g->pauseRequest -= dt;
      if (CanEnter()) {
        g->pauseRequest = 0;
        Enter();
        g->fromPause = true;
        return;
      }
      if (g->pauseRequest <= 0)
        U2LOG("pause menu: photo mode not opened (flow=%d paused=%d)", GameFlowState(), IsGamePaused());
    }
    // L3 + R3 (Start would also open the game's pause menu).
    bool combo = g->cfg.padCombo && g->input.PadDown(PadLS) && g->input.PadDown(PadRS) &&
                 (g->input.PadPressed(PadLS) || g->input.PadPressed(PadRS));
    if (g->input.KeyPressed(g->cfg.hotkey) || combo) {
      if (CanEnter()) Enter();
      else U2LOG("hotkey ignored: flow=%d paused=%d network=%d device=%p", GameFlowState(), IsGamePaused(), IsNetworkSession(), Device());
    }
    return;
  }
  // Only the game's real pause state counts here (our own menu screen is up while photo mode is).
  if (!IsInGame() || IsPauseMenuState()) {
    Exit(IsInGame() ? "game paused" : "left gameplay");
    return;
  }
  UpdateAnchor();

  if (g->state == State::Capturing) {
    // An accumulator error mid-capture would otherwise plan the same sample forever.
    if (g->accum.Failed() || (g->capture.Params().supersample && g->bigAccum.Failed()))
      g->capture.Abort("no GPU accumulation");
    if (g->input.KeyPressed(VK_ESCAPE) || g->input.KeyPressed(g->cfg.hotkey) || g->input.PadPressed(PadB)) {
      g->capture.Abort("cancelled");
      // Esc / pad B also reach the FE as Back, possibly a frame later: don't let them exit too.
      g->backGuard = kBackGuardSec;
    }
    if (g->capture.Active() && g->frame.hold) {
      // The tile capture's first frame has not rendered yet (no 3D last frame): wait for it.
    } else if (g->capture.Active()) {
      const bool super = g->capture.Params().supersample;
      // A fresh 1x capture counts from 0 while the preview's mean is still on screen (resetBeforeSample).
      const int count = super ? g->bigAccum.Count() : g->resetBeforeSample ? 0 : g->accum.Count();
      CaptureFrame cf = g->capture.BeginFrame(count, g->lastDt);
      const bool tiles = g->capture.Params().sample.tiles > 1 && !super;
      if (cf.newTile) g->accum.Reset();
      g->frame.sample = cf.sample;
      g->frame.adj = cf.adj;
      g->frame.resolve = cf.resolve;
      g->frame.region = cf.region;
      g->frame.warm = cf.warm;
      // The screen shows the developing mean (1x, 2x) or the held frame (tiles), never the jittered
      // or zoomed samples; the menu with its progress stays live on top. Resolve frames too: the photo is drawn off
      // screen (ResolvePhoto).
      g->frame.show = !tiles;
      g->frame.showHold = tiles;
      // The world stays still on resolve frames: no need to wait for the 1/60 limiter (review 2026-10-22).
      SetUncapped(((cf.sample || cf.warm) && g->capture.Params().shutterSec == 0) || cf.resolve);
      // Shutter samples advance the world by a small, non-zero time step through the normal
      // (unpaused) loop path; otherwise the world stays on the game's own pause path.
      freeze::Hold(cf.nextGameSpeed <= 0);
      Var<float>(addr::GameSpeed) = cf.nextGameSpeed > 0 ? cf.nextGameSpeed : g->saved.gameSpeed;
    } else {
      EndCapture();
    }
    ApplyCamera(g->lastDt);
  } else {
    HandleControls(dt);
    if (g->state == State::Off) return;
    if (g->driving) {
      int fin = PlayerFinishTick();
      if (fin != 0 && fin != g->finishAtDrive) {
        Exit("race finished while driving");
        return;
      }
    }
    if (g->state == State::Active) {
      ApplyCamera(g->lastDt);
      if (g->focusMode == FocusMode::Aim) UpdateAim();
      if (g->driving) {  // the world moves: no accumulation (it would ghost), no uncapped frames
        g->accum.Reset();
        g->frame = {};
        SetUncapped(false);
      } else {
        UpdatePreview(dt);
        PlanProbe();
      }
    }
  }
  if (!menu::Open()) {
    Exit("menu unavailable");
    return;
  }
  menu::SetHidden((g->uiHidden && g->state == State::Active) || g->driving);
  // Esc / pad B cancel a capture here (HandleFrame); reaching the menu as Back they also faded every row out for good
  // (user 2026-10-22: only the grey Focus row was left), so the menu takes no input meanwhile.
  menu::SetInputBlocked(g->state == State::Capturing || g->backGuard > 0);
  menu::Update("Photo Mode", StatusText());
  if (g->racerTags) UpdateTags();
  g->lastDt = dt > 0 ? dt : g->lastDt;
}

// ---- racer tags -----------------------------------------------------------------------------
// A tag over each rival (tags.h), placed in the FE canvas: vertices go to clip space as x * [0x79AC10] - 1,
// -(y * [0x79AC14] - 1) around the canvas centre (fe-rendering.md), so canvas = ndc / those. The Focus Target's is green.
constexpr uintptr_t kFeScaleX = 0x79AC10, kFeScaleY = 0x79AC14;
void UpdateTags() {
  if (!g->viewProjValid) return;
  tags::Begin();
  RacerCar cars[kMaxCars];
  const int n = RacerCars(cars, kMaxCars);
  for (int i = 0; i < n; ++i) {
    if (cars[i].racer <= 0) continue;  // rivals only
    const CarPose car = PoseOf(cars[i].car, CarPosition(cars[i].car));
    constexpr float kAboveRoof = 0.4f;
    const Vec3 p = car.pos + car.axis[2] * (car.boxMax.z + kAboveRoof);
    const Mat4& m = g->viewProj;  // row vectors, as AdjustView / AdjustProj
    const float cw = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    if (cw <= 0.01f) continue;  // behind the camera
    const float cx = (p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0]) / cw;
    const float cy = (p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1]) / cw;
    tags::Show(RacerName(cars[i]).c_str(), cx / Var<float>(kFeScaleX), -cy / Var<float>(kFeScaleY), cars[i].car == g->target);
  }
  tags::End();
}

// ---- hooks ----------------------------------------------------------------------------------
void __cdecl FrameDetour() {
  if (!g->firstFrameDone) {  // every ASI is loaded by now: the mod list, with load addresses, for testers' logs
    g->firstFrameDone = true;
    crash::LogModules();
  }
  Tick();
  oFrame();
}

// Into the current render target (the finished 3D frame under the menu, or a photo's resolve target), after the
// sample was taken and the mean drawn:
// - the Aspect Ratio bars (user 2026-10-21, solid black) and the watermark (user 2026-10-20) in the photo area. For a
//   photo (resolve) at 2x or with tiles the target is one quarter of the whole image, so they are placed in the whole
//   image and shifted by the quarter.
// - the grid, in the photo area on screen only: like the menu, it stays out of the photo.
//   The held tile frame and the samples were taken earlier in this hook, so they never have any of these.
void DrawOverlays(IDirect3DDevice9* dev, bool resolve = false) {
  D3DSURFACE_DESC d;
  if (g->state == State::Off || !d3d::BackBufferDesc(dev, &d)) return;
  const int sw = static_cast<int>(d.Width), sh = static_cast<int>(d.Height);
  int w = sw, h = sh, ox = 0, oy = 0;
  const int tiles = g->capture.Params().sample.tiles;
  if (resolve && tiles > 1) {
    const int t = g->frame.region >= 0 ? g->frame.region : g->capture.Tile();  // quarter q at (q % 2, q / 2)
    ox = (t % tiles) * sw;
    oy = (t / tiles) * sh;
    w *= tiles;
    h *= tiles;
  }
  const int aspect = g->state == State::Capturing ? g->capture.Params().aspect : g->aspect;
  const frame::Rect area = frame::Fit(w, h, aspect);
  frame::Rect bars[4];
  grid::Line quads[4];
  const int nBars = frame::Bars(w, h, area, bars);
  for (int i = 0; i < nBars; ++i) quads[i] = {bars[i].x - ox, bars[i].y - oy, bars[i].w, bars[i].h};
  g->quadOverlay.Draw(dev, quads, nBars, 0xFF000000u);
  if (g->watermark && EnsureWatermark(g->watermark)) {
    const watermark::Rect a{static_cast<float>(area.x), static_cast<float>(area.y), static_cast<float>(area.w),
                            static_cast<float>(area.h)};
    g->wmOverlay.Draw(dev, g->wm[g->watermark].mark, g->wmCorner, a, h, ox, oy);
  }
  if (g->grid && !resolve) {
    grid::Line lines[grid::kMaxLines];
    const int n = grid::Thirds(area.x, area.y, area.w, area.h, grid::Thickness(sh), lines);  // w == sw here
    g->quadOverlay.Draw(dev, lines, n, grid::kLineColour);
  }
}

// A resolve frame's photo piece, off screen: the mean 1:1 (one quarter at 2x) with the bars and the watermark, then
// WidescreenFix's gamma and the readback (Capture::ReadResolved). Before 2026-11-02 it was read from the screen at
// EndScene (FE hidden, frame not presented); with in-game FSAA WidescreenFix clears the screen's edge pixels there, which
// needed repair frames and left seams in 2x photos (tester, 2026-11-01).
void ResolvePhoto(IDirect3DDevice9* dev, Accum& acc) {
  IDirect3DSurface9* target = g->capture.ResolveTarget();
  IDirect3DSurface9* screen = nullptr;
  if (!target || acc.Count() == 0 || FAILED(dev->GetRenderTarget(0, &screen))) return;
  dev->SetRenderTarget(0, target);
  if (g->frame.region >= 0) acc.ShowRegion(dev, QuarterUv(g->frame.region));
  else acc.Show(dev);  // the same size: 1:1
  DrawOverlays(dev, true);
  dev->SetRenderTarget(0, screen);  // the 2D pass sets its own viewport next (0x5CBF40)
  screen->Release();
  g->capture.ReadResolved(dev);
}

// Pre-FE point: the backbuffer holds the 3D scene + game post FX; the 2D pass 0x5CBF40(0) with the
// mirror, letterbox and FE comes next (0x5D2B5A..0x5D2B60). Skipped by the game on frames without 3D.
void __cdecl Render3DDetour() {
  WheelScope wheels;
  // 2x sample frame: the whole 3D pass renders into the 2W x 2H target (Begin/End always pair up
  // inside this call), and the sample is folded in while that target is still bound.
  const bool super = g->state == State::Capturing && g->capture.Params().supersample && g->super.Ready();
  IDirect3DDevice9* dev = Device();
  if (g->frame.probe && g->state == State::Active) {  // focus probe frame N+1 (PlanProbe)
    g->frame.probe = false;
    probe::Result r;
    bool ok = false;
    if (g->probe.Ensure(dev)) {
      g->probe.Begin(dev);
      oRender3D();
      g->probe.End(dev);
      const CarPose car = TargetCarPose();
      ok = g->probe.Read(dev, TargetCar() ? &car : nullptr, g->cfg.verbose, &r);
    } else {
      oRender3D();
    }
    OnProbe(ok, r);
    g->accum.ShowHold(dev);  // the frame held just before: the same view, so nothing blinks
    DrawOverlays(dev);
    return;
  }
  const bool noMsaa = g->state == State::Capturing && g->noMsaa && g->super.Ready();
  // A fresh 1x capture empties the preview's mean only at its first sample, so the screen keeps showing it until then
  // instead of one sharp frame (review 2026-10-22). Sample 1 ignores the old mean (accum.hlsl), Reset only counts.
  if (g->frame.sample && g->resetBeforeSample) {
    g->accum.Reset();
    g->resetBeforeSample = false;
  }
  if ((super || noMsaa) && (g->frame.sample || g->frame.warm)) {
    g->super.Begin(dev);
    oRender3D();
    if (g->frame.sample) (super ? g->bigAccum : g->accum).AddSample(dev);
    const bool warm = g->frame.warm;
    g->frame.sample = g->frame.warm = false;
    g->super.End(dev);
    // Warm-up with no preview mean to show: put the 2x frame on screen (scaled down), or the
    // backbuffer would show its post-Present clear (0x5D2C39) under the menu.
    if (warm && g->accum.Count() == 0) g->super.Blit(dev);
  } else {
    oRender3D();
  }
  if (g->state == State::Off) return;
  if (!g->accum.Created() || !g->accum.Ensure(dev)) return DrawOverlays(dev);  // made by the preview or StartCapture
  Accum& acc = super ? g->bigAccum : g->accum;
  if (g->frame.sample) {
    acc.AddSample(dev);
    g->frame.sample = false;  // one sample per planned frame
  }
  if (g->state == State::Capturing && g->frame.resolve) ResolvePhoto(dev, acc);
  if (g->frame.show && acc.Count() > 0) {
    acc.Show(dev);  // a 2x mean is box-filtered down to the screen
  } else if (super && g->frame.show && g->accum.Count() > 0) {
    g->accum.Show(dev);  // 2x capture before its first sample: keep the 1x preview on screen
  }
  if (g->frame.hold) {  // keep this (preview or live) frame for the tile capture, then start empty
    g->accum.Hold(dev);
    g->accum.Reset();
    g->frame.hold = false;
  }
  if (g->frame.showHold) g->accum.ShowHold(dev);
  if (g->frame.probeHold) {  // focus probe frame N: keep it for the next frame (PlanProbe)
    g->accum.Hold(dev);
    g->frame.probeHold = false;
    if (!g->accum.Failed()) g->probeStage = 1;
  }
  DrawOverlays(dev);
}

// Any other mover that still targets the player camera gets our matrix instead.
void __fastcall SetCameraMatrixDetour(CameraParams* cam, void* edx, bMatrix4* m, float dt) {
  if (g->state != State::Off && cam == PlayerCamera()) {
    *m = g->camMatrix;
    cam->FieldOfView = FocalToGameFov(Focal(), FovCal());
  }
  oSetCameraMatrix(cam, edx, m, dt);
}

void __fastcall CubicMoverUpdateDetour(void* mover, void* edx) {
  if (g->state != State::Off) return;  // frozen while photo mode is open (ExOpts "Freeze Camera")
  oCubicMoverUpdate(mover, edx);
}

void __fastcall ProjDetour(void* plat, void* edx, eView* view, float a, float b, int c) {
  oProj(plat, edx, view, a, b, c);
  if (g->state != State::Off && view && view->Id == 1 && plat) {
    g->viewProj = static_cast<Mat4*>(plat)[2];  // before the sample's jitter below
    g->viewProjValid = true;
  }
  const ProjAdjust* adj = g->state != State::Off && g->frame.sample ? &g->frame.adj : nullptr;
  if (!adj || !view || !IsSampledView(view->Id) || !plat) return;
  // The frustum planes (plat+0xC0, computed at 0x5C80C9 from the unadjusted ViewProj) stay the
  // full view's: a tile's frustum is inside it, so culling only keeps a little extra.
  auto m = static_cast<Mat4*>(plat);  // [0] view, [1] proj, [2] viewproj
  m[0] = AdjustView(m[0], *adj);
  m[1] = AdjustProj(m[1], *adj);
  m[2] = Mul(m[0], m[1]);
}

// While photo mode is up only our menu is drawn (HUD, minimap, messages hidden); H hides it too.
// A package is skipped by detaching its object list (pkg+0x4C) for the call.
// A capture's resolve frame (the one read back into the PNG) hides our menu here too. drawHud = 0 alone is not
// enough: the FE pass 0x5378C0 reads it through the pointer at 0x5378C1, which NFSU2_CameraToolkit repoints to its
// own "Draw HUD" flag (camtk dllmain.cpp:220), so with that mod the menu was drawn into photos (beta feedback
// 2026-10-20: "the photo mode ui visible in end screenshots").
void __fastcall FeRenderDetour(void* feng, void* edx) {
  if (g->state == State::Off) return oFeRender(feng, edx);
  bool hideMenu = menu::IsHidden();
  void* saved[64];
  feng::FEPackage* pkgs[64];
  int n = 0;
  for (auto p = feng::FirstActivePackage(); p && n < 64; p = feng::NextPackage(p)) {
    if (menu::IsMenuPackage(feng::PackageName(p)) && !hideMenu) continue;
    // Tags stay with the menu hidden (H), like the grid; never in a photo.
    if (tags::IsPackage(feng::PackageName(p)) && g->racerTags) continue;
    pkgs[n] = p;
    saved[n] = feng::PackageObjects(p);
    feng::PackageObjects(p) = nullptr;
    ++n;
  }
  oFeRender(feng, edx);
  for (int i = 0; i < n; ++i) feng::PackageObjects(pkgs[i]) = saved[i];
}

// Long exposures (beta feedback 2026-10-31: smoke "accumulates in excess in slow motion", gone once the camera closes).
// The particle step 0x492E10 (thiscall, float dt; 0x60A181) ages particles by a whole number of 1/1024 s, truncated
// (0x492E23..0x492E33, subtracted at 0x49307C), while the float dt moves them. A shutter step is shutter / samples: at
// 128 samples anything up to 1/8 s is under one tick, so during the exposure no smoke aged, and repeated photos kept
// adding to it. While the shutter is open the step runs only every kParticleTicks and gets whole ticks; the total time
// is the same.
constexpr uintptr_t kParticleStep = 0x492E10;
constexpr int kParticleTicks = 4;  // about 4 ms, still several steps per normal 60 Hz frame
using ParticleStepFn = void(__fastcall*)(void*, void*, float);
ParticleStepFn oParticleStep;
void __fastcall ParticleStepDetour(void* mgr, void* edx, float dt) {
  if (!g || g->state != State::Capturing || g->capture.Params().shutterSec <= 0) return oParticleStep(mgr, edx, dt);
  g->particleCarry += dt;
  const int ticks = static_cast<int>(g->particleCarry * 1024);
  if (ticks < kParticleTicks) return;
  const float step = ticks / 1024.0f;
  g->particleCarry -= step;
  oParticleStep(mgr, edx, step);
}

// The player's commands are off while photo mode is open, Drive apart (user 2026-11-01: pad Y bound to reset car reset it).
// SetInputMode binds the driving events to thunk 0x605C50 (cdecl port, id, value, player; 0x606130), which runs them at
// once in the event dispatch: 0xA1 calls ResetCar 0x6009A0 (0x60345E), freeze or not; the game's real pause does not
// guard this either. Presses stop here; releases pass, so held states (look back) still end. The FE has its own callback
// 0x50C630, and pauseentry's call to 0x602BD0 is direct.
constexpr uintptr_t kPlayerEventThunk = 0x605C50;
using PlayerEventFn = void(__cdecl*)(int, int, int, void*);
PlayerEventFn oPlayerEvent;
void __cdecl PlayerEventDetour(int port, int id, int value, void* player) {
  if (g && g->state != State::Off && !g->driving && value != 0) return;
  oPlayerEvent(port, id, value, player);
}

void OnDeviceReset() {
  g->capture.Abort("device reset");
  g->accum.Free();  // DEFAULT-pool targets must be released before the game's Reset
  FreeProbeAndOverlays();
  FreeSupersample();
  g->frame = {};
  SetUncapped(false);
  // The game re-creates its render targets inside this reset (0x5D1C40 -> 0x5BA780 -> 0x5BB840),
  // gated by the effect flags: the blur RTs 0x86EB88/0x86EB0C need motion blur, DOF, bleach or rain
  // (0x5BB852..0x5BB87B); the streak buffers 0x86F2E4/0x86FF70 need streaks or overbright
  // (0x5BB899..0x5BB8A9, else nulled at 0x5BB9D8). With our zeros still in place they would be
  // skipped, and restoring the flags later crashes on the missing target (0x5C6223, 0x5C6BEF).
  // This listener runs before the game's handler (d3d.cpp), so put the player's values back now.
  RestoreCaptureEffects();
  if (g->state != State::Off) {
    Var<int>(addr::g_DepthOfFieldEnable) = g->saved.dofEnable;
    Var<int>(addr::g_MotionBlurEnable) = g->saved.mbEnable;
    Var<int>(addr::RainEnable) = g->saved.rainEnable;
    g->rezeroNativeFx = true;
  }
}
}  // namespace

bool Install() {
  g = new PhotoMode();
  g->cfg.Load();
  g->qualityIdx = ClampInt(g->cfg.defaultQuality, 0, lens::Count(lens::kQualitySamples) - 1);
  QueryPerformanceFrequency(&g->qpcFreq);
  // Park the cursor at the right edge, away from the menu bar on the left, so FE mouse hover can't
  // change the selected row while the mouse steers the camera.
  g->input.SetCapturePoint(0.94f, 0.5f);
  // Hooks first: a failure leaves the game untouched. Data patches only once all of them exist.
  if (!hook::Init()) return false;
  bool ok = true;
  ok &= hook::Create(addr::RenderFrame, &FrameDetour, &oFrame, "RenderFrame");
  ok &= hook::Create(addr::Camera_SetCameraMatrix, &SetCameraMatrixDetour, &oSetCameraMatrix, "Camera::SetCameraMatrix");
  ok &= hook::Create(addr::CubicCameraMover_Update, &CubicMoverUpdateDetour, &oCubicMoverUpdate, "CubicCameraMover::Update");
  ok &= hook::Create(addr::eViewPlatInfo_Update, &ProjDetour, &oProj, "eViewPlatInfo::Update");
  ok &= hook::Create(addr::cFEng_Render, &FeRenderDetour, &oFeRender, "cFEng::Render");
  ok &= hook::Create(addr::Render3D, &Render3DDetour, &oRender3D, "Render3D");
  ok &= d3d::InstallResetHook();
  // Optional: without it a quit with photo mode open can save DOF / motion blur as off (logged, not fatal).
  // Entry bytes from u2re: push ecx; push esi; mov esi, [0x783008].
  if (!hook::EntryIs(kSaveRegistry, "\x51\x56\x8B\x35\x08\x30\x78\x00", 8))
    U2LOG("SaveRegistrySettings 0x5BEEA0: entry bytes differ (another mod?); not hooked");
  else if (!hook::Create(kSaveRegistry, &SaveRegistryDetour, &oSaveRegistry, "SaveRegistrySettings"))
    U2LOG("registry-save hook failed: quitting with photo mode open may save DOF / motion blur as off");
  // Optional: without it 2x photos with Enhanced Contrast on are blurred, moved 1.5 px and wrap at the right and bottom
  // edges (supersample.cpp BleachQuadPassDetour). Entry bytes from u2re: push ebp; mov ebp, esp; and esp, -16;
  // sub esp, 0xC4.
  if (!hook::EntryIs(addr::BleachQuadPass, "\x55\x8B\xEC\x83\xE4\xF0\x81\xEC\xC4\x00\x00\x00", 12))
    U2LOG("BleachQuadPass 0x5C6520: entry bytes differ (another mod?); not hooked");
  else if (!Supersample::InstallBleachFix())
    U2LOG("bleach hook failed: 2x photos with Enhanced Contrast stay soft");
  // Optional: without it long exposures keep the smoke of every step (ParticleStepDetour).
  // Entry bytes from u2re: push ebp; mov ebp, esp; and esp, -16; sub esp, 0x64.
  if (!hook::EntryIs(kParticleStep, "\x55\x8B\xEC\x83\xE4\xF0\x83\xEC\x64", 9))
    U2LOG("particle step 0x492E10: entry bytes differ (another mod?); not hooked");
  else if (!hook::Create(kParticleStep, &ParticleStepDetour, &oParticleStep, "ParticleStep"))
    U2LOG("particle-step hook failed: long exposures may pile up smoke");
  // Optional: without it pad buttons bound to driving commands also act (PlayerEventDetour).
  // Entry bytes from u2re: mov eax, [esp + 0xC]; mov ecx, [esp + 8].
  if (!hook::EntryIs(kPlayerEventThunk, "\x8B\x44\x24\x0C\x8B\x4C\x24\x08", 8))
    U2LOG("player event thunk 0x605C50: entry bytes differ (another mod?); not hooked");
  else if (!hook::Create(kPlayerEventThunk, &PlayerEventDetour, &oPlayerEvent, "PlayerEvent"))
    U2LOG("player-event hook failed: pad buttons may also act on the car");
  // Optional: without it photo mode still opens with its key.
  if (g->cfg.pauseMenuEntry && !pause::Install()) U2LOG("pause menu: no Photo Mode icon (see above)");
  if (!ok) {
    U2LOG("some hooks failed; photo mode disabled");
    return false;  // created hooks are never enabled
  }
  // Footer: user 2026-10-19, "H - Hide/Show UI" at the bottom of the bar; 2026-10-20 "Space - Take Picture" next to it.
  if (!menu::Install(menu::Callbacks{g->cfg.drive ? RowCount : RowCount - 1, &MenuLabel, &MenuValue, &MenuAdjust, &MenuActivate, &MenuBack,
                                     &MenuIsAction, &MenuIsEnabled,
                                     g->padHint ? kFooterPad : kFooterKeys},
                     g->cfg.menuLayout, g->cfg.verbose))
    return false;
  if (!hook::EnableAll()) {
    hook::DisableAll();
    menu::Uninstall();
    U2LOG("enabling hooks failed; photo mode disabled");
    return false;
  }
  if (!freeze::Install()) {  // last: a failure leaves the game untouched
    hook::DisableAll();
    menu::Uninstall();
    U2LOG("world freeze unavailable; photo mode disabled");
    return false;
  }
  d3d::AddResetListener(&OnDeviceReset);
  return true;
}
}  // namespace photomode
