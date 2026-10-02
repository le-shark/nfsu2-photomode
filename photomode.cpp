// NFSU2 Photo Mode: controller (state machine, hooks, menu, camera, capture glue).
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "capture.h"
#include "follow.h"
#include "lens.h"
#include "rig.h"
#include "settings.h"
#include "native_menu.h"
#include "u2sdk/d3d.h"
#include "u2sdk/dev.h"
#include "u2sdk/feng.h"
#include "u2sdk/game.h"
#include "u2sdk/hook.h"
#include "u2sdk/input.h"
#include "u2sdk/log.h"
#include "u2sdk/mem.h"

namespace photomode {
namespace {
using namespace u2;

// ---- world freeze ---------------------------------------------------------------------------
// GameLoop decides at 0x58132C whether the game is paused:
//   0x58132C  movzx eax, al      ; al = paused (RaceCoordinator state 6 = pause menu)
//   0x58132F  cmp eax, ebx       ; ebx = 0
//   0x581331  je  0x581349       ; not paused -> sim block, World::DoTimestep 0x609CE0(scaledDt)
//   paused:   0x601A60(dt=0), store sim dt, jmp 0x581465 (skips the whole simulation block)
// We redirect those 5 bytes to a stub that reports "paused" while photo mode wants the world frozen,
// so the game takes its own pause path. (Tweak_GameSpeed = 0 is NOT equivalent: it still runs
// World::DoTimestep with dt = 0, which left the game permanently frozen after exiting.)
volatile bool g_freezeWorld = false;
constexpr uintptr_t kPauseDecision = 0x58132C, kPauseDecisionReturn = 0x581331;
constexpr uint8_t kPauseDecisionBytes[5] = {0x0F, 0xB6, 0xC0, 0x3B, 0xC3};  // movzx eax,al; cmp eax,ebx

__declspec(naked) void PauseDecisionStub() {
  __asm {
    movzx eax, al
    cmp byte ptr [g_freezeWorld], 0
    je done
    mov eax, 1
  done:
    cmp eax, ebx
    push kPauseDecisionReturn
    ret
  }
}

enum class State { Off, Active, Capturing };
enum Row { RowCamera, RowTarget, RowFocal, RowRoll, RowFocus, RowAperture, RowShutter, RowFollow, RowQuality, RowResolution, RowTake, RowCount };

constexpr float kMinFocusM = 0.3f, kMaxFocusM = 500;
constexpr float kBackGuardSec = 0.3f;  // after a key cancels a capture, the same press must not also exit

struct Saved {
  float gameSpeed;
  int dofEnable, mbEnable, drawHud;
  uint16_t fov;
  uint8_t cursorVisible;
};

// Center / Closest = autofocus on the target car's centre / nearest corner.
enum class FocusMode { Center, Closest, Manual };

struct PhotoMode {
  Settings cfg;
  Input input;
  Rig rig;
  Capture capture;
  State state = State::Off;
  Saved saved{};

  // menu values
  int focalIdx = 10, fstopIdx = lens::kDefaultFStop, shutterIdx = 0, qualityIdx = 1, tiles = 1;
  FocusMode focusMode = FocusMode::Center;
  float focusDist = 5;
  Follow follow = Follow::Rig;  // how the camera follows the target car while the shutter is open
  CamPose captureCam{};         // camera and car when the shutter opened
  CarPose captureCar{};
  bool uiHidden = false;
  std::string toast;
  float toastTime = 0;
  float backGuard = 0;          // seconds left in which Back/close keys are ignored

  Vec3 anchor{};
  uint8_t* target = nullptr;  // orbit target car (World car array entry); null = player
  LARGE_INTEGER lastQpc{}, qpcFreq{};
  float lastDt = 1 / 60.f;
  alignas(16) bMatrix4 camMatrix{};  // world->view matrix applied in the SetCameraMatrix hook
  bool frameArmed = false;    // main-frame EndScene pending
  bool endSceneHooked = false;
  float postExitLog = 0;      // verbose: seconds left to log game state after exit
  int debugShot = 0;
  POINT cursorAtEnter{};
};
PhotoMode* g = nullptr;

// ---- original functions -------------------------------------------------------------------
using SetCameraMatrixFn = void(__fastcall*)(CameraParams*, void*, bMatrix4*, float);
using MoverUpdateFn = void(__fastcall*)(void*, void*);
using ProjFn = void(__fastcall*)(void*, void*, eView*, float, float, int);
using FeRenderFn = void(__fastcall*)(void*, void*);
using FrameFn = void(__cdecl*)();
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
SetCameraMatrixFn oSetCameraMatrix;
MoverUpdateFn oCubicMoverUpdate;
ProjFn oProj;
FeRenderFn oFeRender;
FrameFn oFrame;
EndSceneFn oEndScene;

// ---- helpers --------------------------------------------------------------------------------
float FovCal() { return g->cfg.fovCalibration > 0 ? g->cfg.fovCalibration : kDefaultVertFovScale; }
float Focal() { return lens::kFocals[g->focalIdx]; }
float FStop() { return lens::kFStops[g->fstopIdx]; }

void Toast(const std::string& s, float seconds = 3) {
  g->toast = s;
  g->toastTime = seconds;
  U2LOG("toast: %s", s.c_str());
}

void EnsureOutputDir() { CreateDirectoryA(g->cfg.outputDir.c_str(), nullptr); }

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

// Orbit only: in free mode a new target would drag the camera through the distance clamp.
void CycleTarget(int dir) {
  if (g->rig.mode != RigMode::Orbit) {
    Toast("TARGET: ORBIT CAMERA ONLY");
    return;
  }
  RacerCar cars[kMaxCars];
  int n = RacerCars(cars, kMaxCars);
  if (n < 2) {
    Toast("NO OTHER RACERS");
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
  CamPose cam = RigPose();
  CarPose car = TargetCarPose();
  float d = mode == FocusMode::Closest ? NearestCarDepth(cam, car) : CarCentreDepth(cam, car);
  return d < kMinFocusM ? kMinFocusM : d;
}
float FocusDistance() { return g->focusMode == FocusMode::Manual ? g->focusDist : AutoFocusDistance(g->focusMode); }

// Camera pose for this frame: the rig, or while the shutter is open the shutter-follow pose.
CamPose CurrentPose() {
  if (g->state == State::Capturing) return FollowPose(g->follow, g->captureCam, g->captureCar, TargetCarPose());
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
bool CanEnter() { return IsInGame() && !IsGamePaused() && !IsNetworkSession() && Device(); }

void Enter() {
  CameraParams* cam = PlayerCamera();
  g->saved = Saved{Var<float>(addr::GameSpeed), Var<int>(addr::g_DepthOfFieldEnable), Var<int>(addr::g_MotionBlurEnable),
                   Var<int>(addr::DrawHud), cam->FieldOfView, Var<uint8_t>(addr::FECursorVisible)};
  g->target = nullptr;
  if (g->cfg.verbose) LogCars();
  if (!UpdateAnchor()) g->anchor = ToVec3(cam->Position);
  g->rig.maxDist = g->cfg.maxDistance;
  g->rig.moveSpeed = g->cfg.moveSpeed;
  g->rig.InitFromCamera(ToVec3(cam->Position), ToVec3(cam->Direction), g->anchor);
  g->rig.mode = RigMode::Orbit;
  float focal = GameFovToFocal(cam->FieldOfView, FovCal());
  g->focalIdx = lens::Closest(lens::kFocals, focal);
  g->uiHidden = false;
  g->backGuard = 0;
  g_freezeWorld = true;
  Var<int>(addr::g_DepthOfFieldEnable) = 0;  // turning effects off is always safe
  Var<int>(addr::g_MotionBlurEnable) = 0;
  GetCursorPos(&g->cursorAtEnter);
  Var<uint8_t>(addr::FECursorVisible) = 0;  // hide the FE cursor, like the game's showcase photo shoot (0x4CEE11)
  g->input.SetMouseCapture(true);
  g->state = State::Active;
  ApplyCamera(0);
  // Camera::StopUpdating (set only by mods such as ExOpts) makes SetCameraMatrix ignore us.
  U2LOG("enter: cam pos (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) fov %u (%.0fmm) car (%.1f %.1f %.1f) stopUpdating=%d",
        cam->Position.x, cam->Position.y, cam->Position.z, cam->Direction.x, cam->Direction.y, cam->Direction.z,
        cam->FieldOfView, focal, g->anchor.x, g->anchor.y, g->anchor.z, Var<int>(addr::CameraStopUpdating));
}

void Exit(const char* why) {
  if (g->state == State::Off) return;
  g->capture.Abort("PHOTO MODE CLOSED");
  g_freezeWorld = false;
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
  if (lens::kShutters[g->shutterIdx] > 0 && g->tiles > 1) {
    Toast("SHUTTER BLUR NEEDS 1X RESOLUTION");
    return;
  }
  if (g->capture.Saving()) {
    Toast("STILL SAVING PREVIOUS PICTURE");
    return;
  }
  CaptureParams p;
  p.samples = lens::kQualitySamples[g->qualityIdx];
  p.tiles = g->tiles;
  p.lensRadius = lens::ApertureRadiusM(Focal(), FStop());
  p.focusDist = FocusDistance();
  p.shutterSec = lens::kShutters[g->shutterIdx];
  p.antialias = g->cfg.antialias;
  p.outPath = NewPicturePath();
  std::string err;
  if (!g->capture.Start(Device(), p, &err)) {
    Toast(err);
    return;
  }
  // Clean frame: FE off (HUD + our UI) via drawHud. Native DOF/MB are already off in photo mode.
  Var<int>(addr::DrawHud) = 0;
  g->captureCam = RigPose();
  g->captureCar = TargetCarPose();
  g->state = State::Capturing;
}

void EndCapture() {
  // The camera stays where the exposure ended instead of snapping back to where it started.
  CamPose end = CurrentPose();
  g->rig.SetPose(end.eye, end.fwd, end.up, g->anchor);
  g_freezeWorld = true;
  Var<float>(addr::GameSpeed) = g->saved.gameSpeed;
  Var<int>(addr::DrawHud) = g->saved.drawHud;
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
      break;
    case RowFocal: g->focalIdx = ClampInt(g->focalIdx + dir, 0, lens::Count(lens::kFocals) - 1); break;
    case RowRoll: g->rig.roll = Clamp(std::round(g->rig.roll) + dir, -kMaxRoll, kMaxRoll); break;
    case RowFocus:
      if (g->focusMode != FocusMode::Manual) {
        g->focusDist = AutoFocusDistance(g->focusMode);
        g->focusMode = FocusMode::Manual;
      }
      g->focusDist = Clamp(g->focusDist * (dir > 0 ? 1.08f : 1 / 1.08f), kMinFocusM, kMaxFocusM);
      break;
    case RowAperture: g->fstopIdx = ClampInt(g->fstopIdx + dir, 0, lens::Count(lens::kFStops) - 1); break;
    case RowShutter: g->shutterIdx = ClampInt(g->shutterIdx + dir, 0, lens::Count(lens::kShutters) - 1); break;
    case RowFollow: g->follow = static_cast<Follow>(Wrap(static_cast<int>(g->follow) + dir, static_cast<int>(Follow::Count))); break;
    case RowQuality: g->qualityIdx = Wrap(g->qualityIdx + dir, lens::Count(lens::kQualitySamples)); break;
    case RowResolution: g->tiles = ClampInt(g->tiles + dir, 1, lens::kMaxTiles); break;
    default: break;
  }
}

bool IsActionRow(int row) { return row == RowTake; }

void Activate(int row) {
  if (g->state != State::Active) return;
  if (row == RowTake) StartCapture();
  else if (row == RowFocus) {  // Enter cycles Center -> Closest -> Manual (at the current distance)
    switch (g->focusMode) {
      case FocusMode::Center: g->focusMode = FocusMode::Closest; break;
      case FocusMode::Closest:
        g->focusDist = AutoFocusDistance(FocusMode::Closest);
        g->focusMode = FocusMode::Manual;
        break;
      case FocusMode::Manual: g->focusMode = FocusMode::Center; break;
    }
  }
  else Adjust(row, 1);
}

// Row texts for the native menu (native_menu.cpp draws them into UI_PauseOptions.fng).
std::string RowLabel(int row) {
  static const char* names[RowCount] = {"Camera",  "Target", "Focal Length", "Roll",       "Focus",     "Aperture",
                                        "Shutter", "Shutter Follow", "Quality",    "Resolution", "Take Picture"};
  return row >= 0 && row < RowCount ? names[row] : "";
}

std::string RowValue(int row) {
  char b[64] = "";
  switch (row) {
    case RowCamera: return g->rig.mode == RigMode::Orbit ? "Orbit" : "Free";
    case RowTarget: return TargetName();
    case RowFocal: std::snprintf(b, sizeof b, "%.0f mm", Focal()); break;
    case RowRoll: std::snprintf(b, sizeof b, "%+.0f", g->rig.roll); break;
    case RowFocus:
      if (g->focusMode == FocusMode::Manual) std::snprintf(b, sizeof b, "%.1f m", g->focusDist);
      else
        std::snprintf(b, sizeof b, "%s %.1f m", g->focusMode == FocusMode::Center ? "Center" : "Closest",
                      AutoFocusDistance(g->focusMode));
      break;
    case RowAperture: lens::FormatFStop(b, sizeof b, FStop()); break;
    case RowShutter: return lens::kShutterNames[g->shutterIdx];
    case RowFollow: return FollowName(g->follow);
    case RowQuality: return lens::kQualityNames[g->qualityIdx];
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
  if (g->state == State::Capturing) g->capture.Abort("CANCELLED");
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
  g->rig.Update(ri, dt, g->anchor);
}

// Grab the finished frame (after WidescreenFix's own EndScene work, before Present).
HRESULT __stdcall EndSceneDetour(IDirect3DDevice9* dev) {
  if (g->frameArmed) {
    g->frameArmed = false;
    dev::OnEndScene(dev);
    if (g->state == State::Capturing && dev == Device()) g->capture.OnEndScene(dev);
  }
  return oEndScene(dev);
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
    d3d::HookDeviceMethod(d3d::vt::EndScene, reinterpret_cast<void*>(&EndSceneDetour), reinterpret_cast<void**>(&oEndScene),
                          "IDirect3DDevice9::EndScene");
    g->endSceneHooked = true;
  }
  g->frameArmed = true;
  menu::Tick();
  g->input.Update();
  if (g->toastTime > 0) g->toastTime -= dt;
  if (g->backGuard > 0) g->backGuard -= dt;
  if (std::string m = g->capture.TakeMessage(); !m.empty()) Toast(m, 4);

  if (g->input.KeyPressed(VK_F12)) {  // debug: save the displayed frame incl. HUD/UI
    EnsureOutputDir();
    dev::RequestShot(g->cfg.outputDir + "\\debug_" + std::to_string(++g->debugShot) + ".png", 2);
  }
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
    if (g->input.KeyPressed(VK_ESCAPE) || g->input.KeyPressed(g->cfg.hotkey) || g->input.PadPressed(PadB)) {
      g->capture.Abort("CANCELLED");
      // Esc / pad B also reach the FE as Back, possibly a frame later: don't let them exit too.
      g->backGuard = kBackGuardSec;
    }
    if (g->capture.Active()) {
      float speed = 0;
      g->capture.BeginFrame(g->lastDt, &speed);
      // Shutter samples advance the world by a small, non-zero time step through the normal
      // (unpaused) loop path; otherwise the world stays on the game's own pause path.
      g_freezeWorld = speed <= 0;
      Var<float>(addr::GameSpeed) = speed > 0 ? speed : g->saved.gameSpeed;
    } else {
      EndCapture();
    }
    ApplyCamera(g->lastDt);
  } else {
    HandleControls(dt);
    if (g->state == State::Off) return;
    if (g->state == State::Active) ApplyCamera(g->lastDt);
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
  const ProjAdjust* adj = g->state == State::Capturing ? g->capture.Adjust() : nullptr;
  if (!adj || !view || view->Id != 1 || !plat) return;
  auto m = static_cast<Mat4*>(plat);  // [0] view, [1] proj, [2] viewproj
  m[0] = AdjustView(m[0], *adj);
  m[1] = AdjustProj(m[1], *adj);
  m[2] = Mul(m[0], m[1]);
}

// While photo mode is up only our menu is drawn (HUD, minimap, messages hidden); H hides it too.
// A package is skipped by detaching its object list (pkg+0x4C) for the call.
void __fastcall FeRenderDetour(void* feng, void* edx) {
  if (g->state == State::Off) return oFeRender(feng, edx);
  bool hideMenu = menu::IsHidden();
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

void OnDeviceReset() { g->capture.Abort("DEVICE RESET"); }
}  // namespace

bool Install() {
  g = new PhotoMode();
  g->cfg.Load();
  g->qualityIdx = ClampInt(g->cfg.defaultQuality, 0, lens::Count(lens::kQualitySamples) - 1);
  QueryPerformanceFrequency(&g->qpcFreq);
  // Park the cursor at the right edge, away from the menu bar on the left, so FE mouse hover can't
  // change the selected row while the mouse steers the camera.
  g->input.SetCapturePoint(0.94f, 0.5f);
  if (std::memcmp(reinterpret_cast<void*>(kPauseDecision), kPauseDecisionBytes, 5) != 0) {
    U2LOG("pause decision @ %08X has unexpected bytes (another mod?); photo mode disabled", static_cast<unsigned>(kPauseDecision));
    return false;
  }
  // Hooks first: a failure leaves the game untouched. Data patches only once all of them exist.
  if (!hook::Init()) return false;
  bool ok = true;
  ok &= hook::Create(addr::RenderFrame, &FrameDetour, &oFrame, "RenderFrame");
  ok &= hook::Create(addr::Camera_SetCameraMatrix, &SetCameraMatrixDetour, &oSetCameraMatrix, "Camera::SetCameraMatrix");
  ok &= hook::Create(addr::CubicCameraMover_Update, &CubicMoverUpdateDetour, &oCubicMoverUpdate, "CubicCameraMover::Update");
  ok &= hook::Create(addr::eViewPlatInfo_Update, &ProjDetour, &oProj, "eViewPlatInfo::Update");
  ok &= hook::Create(addr::cFEng_Render, &FeRenderDetour, &oFeRender, "cFEng::Render");
  ok &= d3d::InstallResetHook();
  if (!ok) {
    U2LOG("some hooks failed; photo mode disabled");
    return false;  // created hooks are never enabled
  }
  if (!menu::Install(menu::Callbacks{RowCount, &RowLabel, &RowValue, &Adjust, &Activate, &MenuBack, &IsActionRow},
                     g->cfg.menuLayout, g->cfg.verbose))
    return false;
  mem::MakeJmp(kPauseDecision, reinterpret_cast<void*>(&PauseDecisionStub));
  if (!hook::EnableAll()) {
    hook::DisableAll();
    mem::WriteBytes(kPauseDecision, kPauseDecisionBytes, sizeof kPauseDecisionBytes);
    menu::Uninstall();
    U2LOG("enabling hooks failed; photo mode disabled");
    return false;
  }
  d3d::AddResetListener(&OnDeviceReset);
  U2LOG("pause decision @ %08X redirected", static_cast<unsigned>(kPauseDecision));
  return true;
}
}  // namespace photomode
