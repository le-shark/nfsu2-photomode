// NFSU2 Photo Mode: controller (state machine, hooks, menu, camera, capture glue).
#include <windows.h>
#include <shlobj.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "accum.h"
#include "capture.h"
#include "follow.h"
#include "lens.h"
#include "rig.h"
#include "sampling.h"
#include "supersample.h"
#include "settings.h"
#include "u2sdk/optmenu.h"
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
enum Row { RowCamera, RowFocal, RowRoll, RowTarget, RowFocusMode, RowFocus, RowAperture, RowShutter, RowFollow, RowResolution, RowTake, RowCount };

constexpr float kMinFocusM = 0.3f, kMaxFocusM = 500;
constexpr float kBackGuardSec = 0.3f;
constexpr float kMinFrameLimit = 1 / 1800.f;  // see addr::FrameLimit  // after a key cancels a capture, the same press must not also exit

struct Saved {
  float gameSpeed;
  int dofEnable, mbEnable, drawHud;
  uint16_t fov;
  uint8_t cursorVisible;
};

// What the Render3D post-hook does this frame (decided in Tick, before the frame renders).
struct FramePlan {
  bool sample = false;  // fold this frame into the accumulator (projection jittered by adj)
  bool show = false;    // draw the accumulated image over the scene
  bool shown = false;   // set by the Render3D hook when it drew the mean this frame
  bool hold = false;    // tile capture start: keep the frame on screen, then empty the mean
  bool showHold = false;// tile capture: draw the held frame over the zoomed tile sample
  bool resolve = false; // capture resolve frame: read back at EndScene, never presented
  bool warm = false;    // 2x capture warm-up: render into the 2x targets, don't sample
  int region = -1;      // supersample resolve: quarter of the 2x mean to show
  ProjAdjust adj;
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
  int focalIdx = 10, fstopIdx = lens::kDefaultFStop, shutterIdx = 0, qualityIdx = 1, tiles = 1;
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
  bool openedBefore = false;    // Camera Mode, Focal Length and Roll are kept from the previous opening (Enter)
  std::string toast;
  float toastTime = 0;
  float backGuard = 0;          // seconds left in which Back/close keys are ignored

  Vec3 anchor{};
  // Free mode: the car the camera's MaxDistance range stays around (the target when Free mode began), so a new
  // Focus Target only changes what Center / Closest focus on (user 2026-10-19).
  uint8_t* rangeCar = nullptr;
  uint8_t* target = nullptr;  // orbit target car (World car array entry); null = player
  LARGE_INTEGER lastQpc{}, qpcFreq{};
  float lastDt = 1 / 60.f;
  alignas(16) bMatrix4 camMatrix{};  // world->view matrix applied in the SetCameraMatrix hook
  bool frameArmed = false;    // main-frame EndScene pending
  bool endSceneHooked = false;
  bool captureHooks = false;  // EndScene and Present both hooked
  float postExitLog = 0;      // verbose: seconds left to log game state after exit
  POINT cursorAtEnter{};

  // GPU accumulation (live preview and capture)
  Accum accum{"accum"};
  // 2x photos: the 3D pass renders into a 2W x 2H target; its own accumulator at that size.
  Supersample super;
  Accum bigAccum{"accum2x"};
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
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
SetCameraMatrixFn oSetCameraMatrix;
MoverUpdateFn oCubicMoverUpdate;
ProjFn oProj;
FeRenderFn oFeRender;
FrameFn oFrame;
FrameFn oRender3D;
EndSceneFn oEndScene;
PresentFn oPresent;

// ---- helpers --------------------------------------------------------------------------------
float FovCal() { return g->cfg.fovCalibration > 0 ? g->cfg.fovCalibration : kDefaultVertFovScale; }
float Focal() { return lens::kFocals[g->focalIdx]; }
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

std::string TargetName() {
  RacerCar cars[kMaxCars];
  int i = TargetIndex(cars, RacerCars(cars, kMaxCars));
  if (i < 0 || cars[i].racer == 0) return "You";
  char b[32];
  std::snprintf(b, sizeof b, "Racer %d", cars[i].racer);
  return b;
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
CarPose TargetCarPose() {
  CarPose c{g->anchor, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  if (uint8_t* car = TargetCar()) {
    Vec3 axes[3];
    CarAxes(car, axes);
    c.pos = CarPosition(car);
    if (std::fabs(Length(axes[0]) - 1) < 0.1f && std::fabs(Length(axes[2]) - 1) < 0.1f)
      for (int r = 0; r < 3; ++r) c.axis[r] = axes[r];
    CarLocalBox(car, &c.boxMin, &c.boxMax);  // the car's own body box; the fallback box stays without one
  }
  return c;
}

CamPose RigPose() {
  Vec3 eye, target, up;
  g->rig.Compute(g->anchor, &eye, &target, &up);
  return {eye, Normalize(target - eye), up};
}

// Autofocus: the car's centre, or the nearest corner of an approximate car box (photographers
// focus on the near headlight).
float AutoFocusDistance(FocusMode mode) {
  if (mode == FocusMode::Aim) return g->aimDist;
  CamPose cam = RigPose();
  CarPose car = TargetCarPose();
  float d = mode == FocusMode::Closest ? NearestCarDepth(cam, car) : CarCentreDepth(cam, car);
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
  if (g->cfg.verbose && (source != g->aimLogged || std::fabs(g->aimDist - g->aimLoggedDist) > 0.1f * g->aimLoggedDist)) {
    static const char* names[] = {"none", "world", "car"};
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

void Enter() {
  g->rangeCar = nullptr;
  CameraParams* cam = PlayerCamera();
  g->saved = Saved{Var<float>(addr::GameSpeed), Var<int>(addr::g_DepthOfFieldEnable), Var<int>(addr::g_MotionBlurEnable),
                   Var<int>(addr::DrawHud), cam->FieldOfView, Var<uint8_t>(addr::FECursorVisible)};
  g->target = nullptr;
  if (g->cfg.verbose) LogCars();
  if (!UpdateAnchor()) g->anchor = ToVec3(cam->Position);
  g->rig.maxDist = g->cfg.maxDistance;
  g->rig.moveSpeed = g->cfg.moveSpeed;
  // Beta feedback 2026-10-20 ("the camera settings reset everytime I deactivate it"): Camera Mode, Focal Length and
  // Roll stay as they were left (the other rows always did); only the first opening takes the game camera's focal
  // length. The camera itself still starts where the game's camera is.
  const float roll = g->rig.roll;
  g->rig.InitFromCamera(ToVec3(cam->Position), ToVec3(cam->Direction), g->anchor);
  float focal = GameFovToFocal(cam->FieldOfView, FovCal());
  if (g->openedBefore) {
    g->rig.roll = roll;
  } else {
    g->focalIdx = lens::Closest(lens::kFocals, focal);
    g->openedBefore = true;
  }
  g->uiHidden = false;
  g->backGuard = 0;
  freeze::Hold(true);
  Var<int>(addr::g_DepthOfFieldEnable) = 0;  // turning effects off is always safe
  Var<int>(addr::g_MotionBlurEnable) = 0;
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
  U2LOG("enter: cam pos (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) fov %u (%.0fmm) car (%.1f %.1f %.1f) stopUpdating=%d",
        cam->Position.x, cam->Position.y, cam->Position.z, cam->Direction.x, cam->Direction.y, cam->Direction.z,
        cam->FieldOfView, focal, g->anchor.x, g->anchor.y, g->anchor.z, Var<int>(addr::CameraStopUpdating));
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
  if (g->savedRain >= 0) Var<int>(addr::RainEnable) = g->savedRain;
  oSaveRegistry();
  Var<int>(addr::g_DepthOfFieldEnable) = dof, Var<int>(addr::g_MotionBlurEnable) = mb;
  Var<int>(addr::LightStreaksEnable) = streaks, Var<int>(addr::HorizonFogEnable) = fog, Var<int>(addr::RainEnable) = rain;
  U2LOG("registry save while photo mode is open: the player's DOF %d / motion blur %d were written", g->saved.dofEnable,
        g->saved.mbEnable);
}

void FreeSupersample() {
  g->super.Free();
  g->bigAccum.Free();
}

void Exit(const char* why) {
  if (g->state == State::Off) return;
  g->capture.Abort("photo mode closed");
  g->frame = {};
  g->accum.Free();  // the targets are recreated on the next enter
  RestoreCaptureEffects();
  FreeSupersample();
  SetUncapped(false);
  Var<int>(addr::MirrorOverride) = g->savedMirror;
  freeze::Hold(false);
  Var<float>(addr::GameSpeed) = g->saved.gameSpeed;
  Var<int>(addr::g_DepthOfFieldEnable) = g->saved.dofEnable;
  Var<int>(addr::g_MotionBlurEnable) = g->saved.mbEnable;
  Var<int>(addr::DrawHud) = g->saved.drawHud;
  PlayerCamera()->FieldOfView = g->saved.fov;
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
  if (!g->captureHooks) {
    Toast("Capture unavailable (see the log)");
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
  if (p.sample.tiles > 1) {
    // 2x: supersampled in one pass (motion blur works); tiles only when the targets don't fit.
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
  std::string err;
  if (!g->capture.Start(Device(), p, &err)) {
    FreeSupersample();
    Toast(err);
    return;
  }
  if (p.sample.tiles > 1) SetCaptureEffects(!p.supersample);
  if (p.supersample) g->bigAccum.Reset();
  // A 1x photo without shutter is exactly what the preview accumulates: continue from its samples
  // (instant when it has converged). Anything else starts from an empty mean.
  g->captureContinued = p.sample.tiles == 1 && p.shutterSec == 0 && CurrentKey() == g->accKey;
  // Tiles empty the mean in the Render3D hook, after holding the frame on screen (see FramePlan).
  if (!g->captureContinued && p.sample.tiles == 1) g->accum.Reset();
  g->captureFromSample = g->captureContinued ? g->accum.Count() : 0;
  QueryPerformanceCounter(&g->captureStart);
  // Started from the controls (Space / pad X), nothing else plans this frame: keep showing the mean
  // so a converged preview doesn't flash the sharp scene for a frame.
  g->frame = {};
  g->frame.show = true;
  g->frame.hold = p.sample.tiles > 1 && !p.supersample;
  g->captureCam = RigPose();
  g->captureCar = TargetCarPose();
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
    case RowFocal: g->focalIdx = ClampInt(g->focalIdx + dir, 0, lens::Count(lens::kFocals) - 1); break;
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
      g->focusDist = Clamp(g->focusDist * (dir > 0 ? 1.08f : 1 / 1.08f), kMinFocusM, kMaxFocusM);
      break;
    case RowAperture: g->fstopIdx = ClampInt(g->fstopIdx + dir, 0, lens::Count(lens::kFStops) - 1); break;
    case RowShutter: g->shutterIdx = ClampInt(g->shutterIdx + dir, 0, lens::Count(lens::kShutters) - 1); break;
    case RowFollow: g->follow = static_cast<Follow>(Wrap(static_cast<int>(g->follow) + dir, static_cast<int>(Follow::Count))); break;
    case RowResolution: g->tiles = ClampInt(g->tiles + dir, 1, lens::kMaxTiles); break;
    default: break;
  }
}

bool IsActionRow(int row) { return row == RowTake; }
// The focus distance is shown live in every mode but only editable in Manual (native disabled row: grey, skipped).
bool IsRowEnabled(int row) { return row != RowFocus || g->focusMode == FocusMode::Manual; }

void Activate(int row) {
  if (g->state != State::Active) return;
  if (row == RowTake) StartCapture();
  else Adjust(row, 1);
}

// Row texts for the native menu (u2sdk optmenu draws them into UI_PauseOptions.fng).
std::string RowLabel(int row) {
  static const char* names[RowCount] = {"Camera Mode", "Focal Length", "Roll",     "Focus Target",
                                        "Focus Mode", "Focus",        "Aperture", "Shutter Speed",
                                        "Shutter Follow", "Resolution", "Take Picture"};
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
      else std::snprintf(b, sizeof b, "%.1f m", FocusDistance());
      break;
    case RowAperture: lens::FormatFStop(b, sizeof b, FStop()); break;
    case RowShutter: return lens::kShutterNames[g->shutterIdx];
    case RowFollow: return FollowName(g->follow);
    case RowResolution: std::snprintf(b, sizeof b, "%dx", g->tiles); break;
    default: break;
  }
  return b;
}

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
  else if (g->backGuard <= 0) Exit("menu back");
}

// ---- per-frame ------------------------------------------------------------------------------
void HandleControls(float dt) {
  Input& in = g->input;
  const PadState& pad = in.Pad();

  if (g->backGuard <= 0 && (in.KeyPressed(g->cfg.hotkey) || in.PadPressed(PadB))) {
    Exit("user");
    return;
  }
  if (in.KeyPressed('H') || in.PadPressed(PadY)) g->uiHidden = !g->uiHidden;
  if (in.KeyPressed(VK_SPACE) || in.PadPressed(PadX)) {
    StartCapture();
    return;
  }
  // Menu navigation (arrows / Enter / Esc, D-pad / A / B) is handled by the game's own FE.
  if (int w = in.Wheel()) Adjust(RowFocal, w);
  if (in.KeyPressed(VK_OEM_4)) CycleTarget(-1);  // [ ] (pad: the Target row)
  if (in.KeyPressed(VK_OEM_6)) CycleTarget(1);

  RigInput ri;
  auto key = [&](int vk) { return in.KeyDown(vk) ? 1.f : 0.f; };
  auto button = [&](uint16_t b) { return in.PadDown(b) ? 1.f : 0.f; };
  ri.moveForward = key('W') - key('S') + pad.ly;
  ri.moveRight = key('D') - key('A') + pad.lx;
  ri.moveUp = key('E') - key('Q') + pad.rt - pad.lt;
  ri.speedMul = (in.KeyDown(VK_SHIFT) || in.PadDown(PadLS)) ? 4.f : ((in.KeyDown(VK_CONTROL) || in.PadDown(PadRS)) ? 0.25f : 1.f);
  // Narrow lenses get slower look so framing stays controllable.
  float zoomScale = Clamp(35.f / Focal(), 0.15f, 1.5f);
  float ySign = g->cfg.invertY ? -1.f : 1.f;
  ri.lookYawDeg = (-in.MouseDX() * g->cfg.mouseSensitivity - pad.rx * g->cfg.stickLookSpeed * dt) * zoomScale;
  ri.lookPitchDeg = (-in.MouseDY() * g->cfg.mouseSensitivity + pad.ry * g->cfg.stickLookSpeed * dt) * zoomScale * ySign;
  ri.rollDeg = (key('C') - key('Z') + button(PadRB) - button(PadLB)) * g->cfg.rollSpeed * dt;
  // Free mode keeps its range around rangeCar once the target changed (CycleTarget); else around the target.
  Vec3 range = g->rig.mode == RigMode::Free && g->rangeCar ? CarPosition(g->rangeCar) : g->anchor;
  g->rig.Update(ri, dt, range);
}

// Grab the finished frame (after WidescreenFix's own EndScene work, before Present).
HRESULT __stdcall EndSceneDetour(IDirect3DDevice9* dev) {
  if (g->frameArmed) {
    g->frameArmed = false;
    // Read back only a resolve frame whose mean was really drawn (Render3D ran this frame).
    if (g->state == State::Capturing && dev == Device() && g->frame.shown) g->capture.OnEndScene(dev);
  }
  return oEndScene(dev);
}

// A capture's resolve frame (the mean of a tile, FE hidden for the readback) is not presented: the
// screen keeps the previous frame with the menu. Safe to skip: the game only checks Present for
// DEVICELOST (0x5D2BC9) and calls TestCooperativeLevel at the start of every frame (0x5D2AF5 ->
// reset handler 0x5D1C40).
HRESULT __stdcall PresentDetour(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
  // Gated on the frame plan, not capture.Active(): the last resolve's EndScene finishes the capture
  // before this frame's Present.
  if (g->state == State::Capturing && g->frame.resolve && dev == Device())
    return S_OK;  // == D3D_OK
  return oPresent(dev, src, dst, wnd, dirty);
}

void LogPostExit(float dt) {
  float before = g->postExitLog;
  g->postExitLog -= dt;
  if (static_cast<int>(before) == static_cast<int>(g->postExitLog) && g->postExitLog > 0) return;
  auto rc = Var<int*>(addr::pRaceCoordinator);
  U2LOG("post-exit: flow=%d speed=%.2f hud=%d helpBar=%d rc=%d stop=%d", GameFlowState(), Var<float>(addr::GameSpeed),
        Var<int>(addr::DrawHud), Var<uint8_t>(addr::PCHelpBarUp), rc ? *rc : -1, Var<int>(addr::CameraStopUpdating));
}

void Tick() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  float dt = g->lastQpc.QuadPart ? static_cast<float>(now.QuadPart - g->lastQpc.QuadPart) / g->qpcFreq.QuadPart : 1 / 60.f;
  g->lastQpc = now;
  dt = Clamp(dt, 0.0f, 0.1f);

  if (!g->endSceneHooked && Device()) {
    // One attempt only; failures are logged by hook::Create.
    bool end = d3d::HookDeviceMethod(d3d::vt::EndScene, reinterpret_cast<void*>(&EndSceneDetour),
                                     reinterpret_cast<void**>(&oEndScene), "IDirect3DDevice9::EndScene");
    bool present = d3d::HookDeviceMethod(d3d::vt::Present, reinterpret_cast<void*>(&PresentDetour),
                                         reinterpret_cast<void**>(&oPresent), "IDirect3DDevice9::Present");
    // A capture needs both: without EndScene the resolve frames never read back, and the Present skip would hold
    // the screen for good (2026-10-18 review).
    g->captureHooks = end && present;
    if (!g->captureHooks) U2LOG("capture unavailable: EndScene %s, Present %s", end ? "ok" : "FAILED", present ? "ok" : "FAILED");
    g->endSceneHooked = true;
  }
  g->frameArmed = true;
  if (g->rezeroNativeFx) {  // after a device reset (OnDeviceReset); the reset frame itself draws nothing
    g->rezeroNativeFx = false;
    if (g->state != State::Off) {
      Var<int>(addr::g_DepthOfFieldEnable) = 0;
      Var<int>(addr::g_MotionBlurEnable) = 0;
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
  if (g->toastTime > 0) g->toastTime -= dt;
  if (g->backGuard > 0) g->backGuard -= dt;
  if (std::string m = g->capture.TakeMessage(); !m.empty()) Toast(m, 4);

  if (g->postExitLog > 0) LogPostExit(dt);
  if (g->state == State::Off) {
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
      CaptureFrame cf = g->capture.BeginFrame((super ? g->bigAccum : g->accum).Count(), g->lastDt);
      const bool tiles = g->capture.Params().sample.tiles > 1 && !super;
      g->frame.shown = false;
      if (cf.newTile) g->accum.Reset();
      g->frame.sample = cf.sample;
      g->frame.adj = cf.adj;
      g->frame.resolve = cf.resolve;
      g->frame.region = cf.region;
      g->frame.warm = cf.warm;
      // The screen shows the developing mean (1x) or the held frame (tiles), never the jittered
      // or zoomed samples; the menu with its progress stays live on top.
      g->frame.show = !tiles || cf.resolve;
      g->frame.showHold = tiles && !cf.resolve;
      // Resolve frame: FE off (our menu) so the mean is read back clean at EndScene.
      Var<int>(addr::DrawHud) = cf.resolve ? 0 : g->saved.drawHud;
      SetUncapped((cf.sample || cf.warm) && g->capture.Params().shutterSec == 0);
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
    if (g->state == State::Active) {
      ApplyCamera(g->lastDt);
      if (g->focusMode == FocusMode::Aim) UpdateAim();
      UpdatePreview(dt);
    }
  }
  if (!menu::Open()) {
    Exit("menu unavailable");
    return;
  }
  menu::SetHidden(g->uiHidden && g->state == State::Active);
  menu::Update("Photo Mode", StatusText());
  g->lastDt = dt > 0 ? dt : g->lastDt;
}

// ---- hooks ----------------------------------------------------------------------------------
void __cdecl FrameDetour() {
  Tick();
  oFrame();
}

// Pre-FE point: the backbuffer holds the 3D scene + game post FX; the 2D pass 0x5CBF40(0) with the
// mirror, letterbox and FE comes next (0x5D2B5A..0x5D2B60). Skipped by the game on frames without 3D.
void __cdecl Render3DDetour() {
  // 2x sample frame: the whole 3D pass renders into the 2W x 2H target (Begin/End always pair up
  // inside this call), and the sample is folded in while that target is still bound.
  const bool super = g->state == State::Capturing && g->capture.Params().supersample && g->super.Ready();
  IDirect3DDevice9* dev = Device();
  if (super && (g->frame.sample || g->frame.warm)) {
    g->super.Begin(dev);
    oRender3D();
    if (g->frame.sample) g->bigAccum.AddSample(dev);
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
  if (!g->accum.Created() || !g->accum.Ensure(dev)) return;  // created by the preview or StartCapture
  Accum& acc = super ? g->bigAccum : g->accum;
  if (g->frame.sample) {
    acc.AddSample(dev);
    g->frame.sample = false;  // one sample per planned frame
  }
  if (g->frame.show && acc.Count() > 0) {
    if (g->frame.region >= 0) acc.ShowRegion(dev, QuarterUv(g->frame.region));  // 2x resolve, 1:1
    else acc.Show(dev);  // a 2x mean is box-filtered down to the screen
    g->frame.shown = true;
  } else if (super && g->frame.show && g->accum.Count() > 0) {
    g->accum.Show(dev);  // 2x capture before its first sample: keep the 1x preview on screen
  }
  if (g->frame.hold) {  // keep this (preview or live) frame for the tile capture, then start empty
    g->accum.Hold(dev);
    g->accum.Reset();
    g->frame.hold = false;
  }
  if (g->frame.showHold) g->accum.ShowHold(dev);
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
  bool hideMenu = menu::IsHidden() || (g->state == State::Capturing && g->frame.resolve);
  void* saved[64];
  feng::FEPackage* pkgs[64];
  int n = 0;
  for (auto p = feng::FirstActivePackage(); p && n < 64; p = feng::NextPackage(p)) {
    if (menu::IsMenuPackage(feng::PackageName(p)) && !hideMenu) continue;
    pkgs[n] = p;
    saved[n] = feng::PackageObjects(p);
    feng::PackageObjects(p) = nullptr;
    ++n;
  }
  oFeRender(feng, edx);
  for (int i = 0; i < n; ++i) feng::PackageObjects(pkgs[i]) = saved[i];
}

void OnDeviceReset() {
  g->capture.Abort("device reset");
  g->accum.Free();  // DEFAULT-pool targets must be released before the game's Reset
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
  {
    const auto* e = reinterpret_cast<const uint8_t*>(kSaveRegistry);
    if (std::memcmp(e, "\x51\x56\x8B\x35\x08\x30\x78\x00", 8) == 0 || e[0] == 0xE9) {
      if (!hook::Create(kSaveRegistry, &SaveRegistryDetour, &oSaveRegistry, "SaveRegistrySettings"))
        U2LOG("registry-save hook failed: quitting with photo mode open may save DOF / motion blur as off");
    } else {
      U2LOG("SaveRegistrySettings 0x5BEEA0: entry bytes differ (another mod?); not hooked");
    }
  }
  if (!ok) {
    U2LOG("some hooks failed; photo mode disabled");
    return false;  // created hooks are never enabled
  }
  // Footer: user 2026-10-19, "H - Hide/Show UI" at the bottom of the bar.
  if (!menu::Install(menu::Callbacks{RowCount, &RowLabel, &RowValue, &Adjust, &Activate, &MenuBack, &IsActionRow, &IsRowEnabled,
                                     "H - Hide/Show UI"},
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
