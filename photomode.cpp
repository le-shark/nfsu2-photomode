// NFSU2 Photo Mode: controller (state machine, hooks, menu, camera, preview, capture glue).
#include <d3d9.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "capture.h"
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
enum Row { RowCamera, RowFocal, RowRoll, RowFocus, RowAperture, RowShutter, RowPan, RowQuality, RowResolution, RowTake, RowCount };

struct Saved {
  float gameSpeed;
  int dofEnable, mbEnable, drawHud;
  CameraParams cam;
};

struct PhotoMode {
  Settings cfg;
  Input input;
  Rig rig;
  Capture capture;
  State state = State::Off;
  Saved saved{};

  // menu values
  int sel = RowFocal;
  int focalIdx = 10, fstopIdx = 3, shutterIdx = 0, qualityIdx = 1, tiles = 1;
  bool focusAuto = true;
  float focusDist = 5;
  bool panWithCar = true;   // free camera follows the car during a shutter exposure
  Vec3 captureAnchor{};
  bool uiHidden = false;
  std::string toast;
  float toastTime = 0;

  Vec3 anchor{};
  LARGE_INTEGER lastQpc{}, qpcFreq{};
  float lastDt = 1 / 60.f;
  alignas(16) bMatrix4 camMatrix{};  // world->view matrix applied in the SetCameraMatrix hook
  bool frameArmed = false;    // main-frame EndScene pending
  bool endSceneHooked = false;
  float postExitLog = 0;      // seconds left to log game state after exit
  int debugShot = 0;
  bool hudFilter = true;      // F11 toggles (debug)
  POINT cursorAtEnter{};
  uint8_t savedCursorFlag = 1;
};
PhotoMode* g = nullptr;

// ---- original functions -------------------------------------------------------------------
using SetCameraMatrixFn = void(__fastcall*)(CameraParams*, void*, bMatrix4*, float);
using ProjFn = void(__fastcall*)(void*, void*, eView*, float, float, int);
using FeRenderFn = void(__fastcall*)(void*, void*);
using FrameFn = void(__cdecl*)();
using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
SetCameraMatrixFn oSetCameraMatrix;
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

bool UpdateAnchor() {
  Vec3 p;
  if (!PlayerCarPosition(&p)) return false;
  g->anchor = p;
  return true;
}

float AutoFocusDistance() {
  Vec3 eye, target, up;
  g->rig.Compute(g->anchor, &eye, &target, &up);
  Vec3 f = Normalize(target - eye);
  float d = Dot(g->anchor + Vec3{0, 0, 0.5f} - eye, f);
  return d < 0.3f ? 0.3f : d;
}
float FocusDistance() { return g->focusAuto ? AutoFocusDistance() : g->focusDist; }

std::string NewPicturePath() {
  CreateDirectoryA(g->cfg.outputDir.c_str(), nullptr);
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
  Vec3 eye, target, up;
  g->rig.Compute(g->anchor, &eye, &target, &up);
  // Free camera + shutter: pan with the car during the exposure (tracking shot).
  if (g->state == State::Capturing && g->rig.mode == RigMode::Free && g->panWithCar) {
    Vec3 d = g->anchor - g->captureAnchor;
    eye += d;
    target += d;
  }
  CreateLookAtMatrix(&g->camMatrix, eye, target, up);
  cam->FieldOfView = FocalToGameFov(Focal(), FovCal());
  oSetCameraMatrix(cam, nullptr, &g->camMatrix, dt);
}

// ---- state transitions ----------------------------------------------------------------------
bool CanEnter() { return IsInGame() && !IsGamePaused() && Device() && PlayerCamera(); }

void Enter() {
  CameraParams* cam = PlayerCamera();
  g->saved = Saved{Var<float>(addr::GameSpeed), Var<int>(addr::g_DepthOfFieldEnable), Var<int>(addr::g_MotionBlurEnable),
                   Var<int>(addr::DrawHud), *cam};
  if (!UpdateAnchor()) g->anchor = ToVec3(cam->Position);
  g->rig.maxDist = g->cfg.maxDistance;
  g->rig.moveSpeed = g->cfg.moveSpeed;
  g->rig.InitFromCamera(ToVec3(cam->Position), ToVec3(cam->Direction), g->anchor);
  g->rig.mode = RigMode::Orbit;
  float focal = GameFovToFocal(cam->FieldOfView, FovCal());
  g->focalIdx = lens::Closest(lens::kFocals, focal);
  g->sel = RowFocal;
  g->uiHidden = false;
  g_freezeWorld = true;
  Var<int>(addr::g_DepthOfFieldEnable) = 0;  // turning effects off is always safe
  Var<int>(addr::g_MotionBlurEnable) = 0;
  GetCursorPos(&g->cursorAtEnter);
  g->savedCursorFlag = Var<uint8_t>(0x7FA29C);
  Var<uint8_t>(0x7FA29C) = 0;  // hide the FE cursor, like the game's showcase photo shoot (0x4CEE11)
  g->input.SetMouseCapture(true);
  g->state = State::Active;
  ApplyCamera(0);
  U2LOG("enter: cam pos (%.1f %.1f %.1f) dir (%.2f %.2f %.2f) fov %u (%.0fmm) car (%.1f %.1f %.1f)", cam->Position.x,
        cam->Position.y, cam->Position.z, cam->Direction.x, cam->Direction.y, cam->Direction.z, cam->FieldOfView, focal,
        g->anchor.x, g->anchor.y, g->anchor.z);
  U2LOG("enter: options dof=%d mb=%d, blur RTs 86EB0C=%p 86EB88=%p 86FFA8=%p 86FF74=%p", g->saved.dofEnable, g->saved.mbEnable,
        Var<void*>(0x86EB0C), Var<void*>(0x86EB88), Var<void*>(0x86FFA8), Var<void*>(0x86FF74));
}

void Exit(const char* why) {
  if (g->state == State::Off) return;
  g->capture.Abort("PHOTO MODE CLOSED");
  CameraParams* cam = PlayerCamera();
  g_freezeWorld = false;
  Var<float>(addr::GameSpeed) = g->saved.gameSpeed;
  Var<int>(addr::g_DepthOfFieldEnable) = g->saved.dofEnable;
  Var<int>(addr::g_MotionBlurEnable) = g->saved.mbEnable;
  Var<int>(addr::DrawHud) = g->saved.drawHud;
  cam->FieldOfView = g->saved.cam.FieldOfView;
  cam->FocalDistance = g->saved.cam.FocalDistance;
  cam->DepthOfField = g->saved.cam.DepthOfField;
  menu::Close();
  g->input.SetMouseCapture(false);
  Var<uint8_t>(0x7FA29C) = g->savedCursorFlag;
  // Put the cursor back: the game/WidescreenFix mouse-look reads cursor deltas.
  SetCursorPos(g->cursorAtEnter.x, g->cursorAtEnter.y);
  g->state = State::Off;
  g->postExitLog = 3;
  U2LOG("exit (%s): speed=%.2f hud=%d", why, Var<float>(addr::GameSpeed), Var<int>(addr::DrawHud));
}

void StartCapture() {
  CaptureParams p;
  p.samples = lens::kQualitySamples[g->qualityIdx];
  p.tiles = g->tiles;
  p.lensRadius = lens::ApertureRadiusM(Focal(), FStop());
  p.focusDist = FocusDistance();
  p.shutterSec = lens::kShutters[g->shutterIdx];
  p.antialias = g->cfg.antialias;
  p.outPath = NewPicturePath();
  if (p.shutterSec > 0 && p.tiles > 1) {
    Toast("SHUTTER BLUR NEEDS 1X RESOLUTION");
    return;
  }
  if (g->capture.Saving()) {
    Toast("STILL SAVING PREVIOUS PICTURE");
    return;
  }
  std::string err;
  if (!g->capture.Start(Device(), p, &err)) {
    Toast(err);
    return;
  }
  // Clean frame: FE off (HUD + our UI) via drawHud. Native DOF/MB are already off in photo mode.
  Var<int>(addr::DrawHud) = 0;
  g->captureAnchor = g->anchor;
  g->state = State::Capturing;
}

void EndCapture() {
  g_freezeWorld = true;
  Var<float>(addr::GameSpeed) = g->saved.gameSpeed;
  Var<int>(addr::DrawHud) = g->saved.drawHud;
  g->state = State::Active;
}

// ---- menu -----------------------------------------------------------------------------------
void Adjust(int row, int dir) {
  auto wrap = [](int v, int n) { return (v % n + n) % n; };
  auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  switch (row) {
    case RowCamera:
      g->rig.SetMode(g->rig.mode == RigMode::Orbit ? RigMode::Free : RigMode::Orbit, g->anchor);
      break;
    case RowFocal: g->focalIdx = clampi(g->focalIdx + dir, 0, lens::Count(lens::kFocals) - 1); break;
    case RowRoll: g->rig.roll = Clamp(std::round(g->rig.roll) + dir, -90, 90); break;
    case RowFocus:
      if (g->focusAuto) {
        g->focusDist = AutoFocusDistance();
        g->focusAuto = false;
      }
      g->focusDist = Clamp(g->focusDist * (dir > 0 ? 1.08f : 1 / 1.08f), 0.3f, 500.f);
      break;
    case RowAperture: g->fstopIdx = clampi(g->fstopIdx + dir, 0, lens::Count(lens::kFStops) - 1); break;
    case RowShutter: g->shutterIdx = clampi(g->shutterIdx + dir, 0, lens::Count(lens::kShutters) - 1); break;
    case RowPan: g->panWithCar = !g->panWithCar; break;
    case RowQuality: g->qualityIdx = wrap(g->qualityIdx + dir, lens::Count(lens::kQualitySamples)); break;
    case RowResolution: g->tiles = clampi(g->tiles + dir, 1, lens::kMaxTiles); break;
    default: break;
  }
}

void Activate(int row) {
  if (row == RowTake) StartCapture();
  else if (row == RowFocus) g->focusAuto = !g->focusAuto;
  else Adjust(row, 1);
}

// Row texts for the native menu (native_menu.cpp draws them into UI_PauseOptions.fng).
std::string RowLabel(int row) {
  static const char* names[RowCount] = {"Camera",  "Focal Length", "Roll",       "Focus",     "Aperture",
                                        "Shutter", "Pan With Car", "Quality",    "Resolution", "Take Picture"};
  return row >= 0 && row < RowCount ? names[row] : "";
}

std::string RowValue(int row) {
  char b[64] = "";
  switch (row) {
    case RowCamera: return g->rig.mode == RigMode::Orbit ? "Orbit" : "Free";
    case RowFocal: std::snprintf(b, sizeof b, "%.0f mm", Focal()); break;
    case RowRoll: std::snprintf(b, sizeof b, "%+.0f", g->rig.roll); break;
    case RowFocus:
      if (g->focusAuto) std::snprintf(b, sizeof b, "Auto (%.1f m)", AutoFocusDistance());
      else std::snprintf(b, sizeof b, "%.1f m", g->focusDist);
      break;
    case RowAperture: lens::FormatFStop(b, sizeof b, FStop()); break;
    case RowShutter: return lens::kShutterNames[g->shutterIdx];
    case RowPan: return g->rig.mode == RigMode::Free ? (g->panWithCar ? "On" : "Off") : "Orbit follows";
    case RowQuality: std::snprintf(b, sizeof b, "%s", lens::kQualityNames[g->qualityIdx]); break;
    case RowResolution: std::snprintf(b, sizeof b, "%dx", g->tiles); break;
    case RowTake: return "";
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
  else Exit("menu back");
}

// ---- per-frame ------------------------------------------------------------------------------
void HandleControls(float dt) {
  Input& in = g->input;
  const PadState& pad = in.Pad();

  if (in.KeyPressed(g->cfg.hotkey) || in.PadPressed(PadB)) {
    Exit("user");
    return;
  }
  if (in.KeyPressed('H') || in.PadPressed(PadY)) g->uiHidden = !g->uiHidden;
  if (in.KeyPressed(VK_SPACE) || in.PadPressed(PadX)) {
    StartCapture();
    return;
  }
  // Menu navigation (arrows / Enter / Esc, D-pad / A / B) is handled by the game's own FE.
  if (int w = in.Wheel()) g->focalIdx = static_cast<int>(Clamp(static_cast<float>(g->focalIdx + w), 0, lens::Count(lens::kFocals) - 1.f));

  RigInput ri;
  auto key = [&](int vk) { return in.KeyDown(vk) ? 1.f : 0.f; };
  ri.moveForward = key('W') - key('S') + pad.ly;
  ri.moveRight = key('D') - key('A') + pad.lx;
  ri.moveUp = key('E') - key('Q') + pad.rt - pad.lt;
  ri.speedMul = (in.KeyDown(VK_SHIFT) || in.PadDown(PadLS)) ? 4.f : ((in.KeyDown(VK_CONTROL) || in.PadDown(PadRS)) ? 0.25f : 1.f);
  // Narrow lenses get slower look so framing stays controllable.
  float zoomScale = Clamp(35.f / Focal(), 0.15f, 1.5f);
  float ySign = g->cfg.invertY ? -1.f : 1.f;
  ri.lookYawDeg = (-in.MouseDX() * g->cfg.mouseSensitivity - pad.rx * g->cfg.stickLookSpeed * dt) * zoomScale;
  ri.lookPitchDeg = (-in.MouseDY() * g->cfg.mouseSensitivity + pad.ry * g->cfg.stickLookSpeed * dt) * zoomScale * ySign;
  float roll = key('C') - key('Z') + (in.PadDown(PadRB) ? 1.f : 0.f) - (in.PadDown(PadLB) ? 1.f : 0.f);
  ri.rollDeg = roll * g->cfg.rollSpeed * dt;
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
  dev::Poll();
  g->input.Update();
  if (g->toastTime > 0) g->toastTime -= dt;
  if (std::string m = g->capture.TakeMessage(); !m.empty()) Toast(m, 4);

  if (g->input.KeyPressed(VK_F11)) {
    g->hudFilter = !g->hudFilter;
    U2LOG("debug: HUD filter %s", g->hudFilter ? "on" : "off");
  }
  if (g->input.KeyPressed(VK_F12)) {  // debug: save the displayed frame incl. HUD/UI
    CreateDirectoryA(g->cfg.outputDir.c_str(), nullptr);
    std::string path = g->cfg.outputDir + "\\debug_" + std::to_string(++g->debugShot) + ".png";
    dev::RequestShot(path, 2);
  }
  if (g->postExitLog > 0) {
    float before = g->postExitLog;
    g->postExitLog -= dt;
    if (static_cast<int>(before) != static_cast<int>(g->postExitLog) || g->postExitLog <= 0) {
      auto rc = Var<int*>(addr::pRaceCoordinator);
      U2LOG("post-exit: flow=%d speed=%.2f hud=%d pauseFE=%d rc=%d stop=%d", GameFlowState(), Var<float>(addr::GameSpeed),
            Var<int>(addr::DrawHud), Var<uint8_t>(addr::PauseFEActive), rc ? *rc : -1, Var<int>(addr::CameraStopUpdating));
    }
  }
  if (g->state == State::Off) {
    // L3 + R3 (Start would also open the game's pause menu).
    bool combo = g->cfg.padCombo && g->input.PadDown(PadLS) && g->input.PadDown(PadRS) &&
                 (g->input.PadPressed(PadLS) || g->input.PadPressed(PadRS));
    if (g->input.KeyPressed(g->cfg.hotkey) || combo) {
      if (CanEnter()) Enter();
      else U2LOG("hotkey ignored: flow=%d paused=%d device=%p", GameFlowState(), IsGamePaused(), Device());
    }
    return;
  }
  // Only the game's real pause state counts here (our own menu screen is up while photo mode is).
  auto rc = Var<int*>(addr::pRaceCoordinator);
  if (!IsInGame() || (rc && *rc == 6)) {
    Exit(IsInGame() ? "game paused" : "left gameplay");
    return;
  }
  UpdateAnchor();

  if (g->state == State::Capturing) {
    if (g->input.KeyPressed(VK_ESCAPE) || g->input.KeyPressed(g->cfg.hotkey) || g->input.PadPressed(PadB))
      g->capture.Abort("CANCELLED");
    if (!g->capture.Active()) {
      EndCapture();
    } else {
      float speed = 0;
      g->capture.BeginFrame(g->lastDt, &speed);
      // Shutter samples advance the world by a small, non-zero time step through the normal
      // (unpaused) loop path; otherwise the world stays on the game's own pause path.
      g_freezeWorld = speed <= 0;
      Var<float>(addr::GameSpeed) = speed > 0 ? speed : g->saved.gameSpeed;
      ApplyCamera(g->lastDt);
    }
  }
  if (g->state == State::Active) {
    HandleControls(dt);
    if (g->state == State::Off) return;
    if (g->state == State::Active) ApplyCamera(g->lastDt);
  }
  if (g->state != State::Off) {
    if (g->uiHidden && g->state == State::Active) menu::Close();
    else if (!menu::IsOpen()) menu::Open();
    menu::Update("Photo Mode", StatusText());
  }
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

using MoverUpdateFn = void(__fastcall*)(void*, void*);
MoverUpdateFn oCubicMoverUpdate;
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

// While photo mode is up only our menu and the PC help bar are drawn (HUD, minimap, messages hidden).
void __fastcall FeRenderDetour(void* feng, void* edx) {
  if (g->state == State::Off || !g->hudFilter) return oFeRender(feng, edx);
  void* saved[64];
  feng::FEPackage* pkgs[64];
  int n = 0;
  for (auto p = feng::FirstActivePackage(); p && n < 64; p = feng::NextPackage(p)) {
    const char* name = feng::PackageName(p);
    if (menu::IsMenuPackage(name) || (name && !_stricmp(name, "UI_PC_Help_Bar.fng"))) continue;
    pkgs[n] = p;
    saved[n] = feng::PackageObjects(p);
    feng::PackageObjects(p) = nullptr;
    ++n;
  }
  oFeRender(feng, edx);
  for (int i = 0; i < n; ++i) feng::PackageObjects(pkgs[i]) = saved[i];
}

void OnDeviceReset() {
  if (g && g->capture.Active()) g->capture.Abort("DEVICE RESET");
}
}  // namespace

bool Install() {
  g = new PhotoMode();
  g->cfg.Load();
  g->qualityIdx = g->cfg.defaultQuality < 0 ? 0 : (g->cfg.defaultQuality > 2 ? 2 : g->cfg.defaultQuality);
  QueryPerformanceFrequency(&g->qpcFreq);
  // Park the cursor at the right edge, away from the menu bar on the left, so FE mouse hover can't
  // change the selected row while the mouse steers the camera.
  g->input.SetCapturePoint(0.94f, 0.5f);
  if (!hook::Init()) return false;
  bool ok = true;
  ok &= hook::Create(0x5D2850, &FrameDetour, &oFrame, "RenderFrame");
  ok &= hook::Create(addr::Camera_SetCameraMatrix, &SetCameraMatrixDetour, &oSetCameraMatrix, "Camera::SetCameraMatrix");
  ok &= hook::Create(0x453570, &CubicMoverUpdateDetour, &oCubicMoverUpdate, "CubicCameraMover::Update");
  ok &= hook::Create(addr::eViewPlatInfo_Update, &ProjDetour, &oProj, "eViewPlatInfo::Update");
  ok &= hook::Create(addr::cFEng_Render, &FeRenderDetour, &oFeRender, "cFEng::Render");
  ok &= d3d::InstallResetHook();
  ok &= menu::Install(menu::Callbacks{RowCount, &RowLabel, &RowValue, &Adjust, &Activate, &MenuBack}, g->cfg.menuLayout);
  // Verify the exact bytes before redirecting the pause decision (movzx eax,al; cmp eax,ebx).
  const uint8_t expect[5] = {0x0F, 0xB6, 0xC0, 0x3B, 0xC3};
  if (std::memcmp(reinterpret_cast<void*>(kPauseDecision), expect, 5) == 0) {
    mem::MakeJmp(kPauseDecision, reinterpret_cast<void*>(&PauseDecisionStub));
    U2LOG("pause decision @ %08X redirected", static_cast<unsigned>(kPauseDecision));
  } else {
    U2LOG("pause decision @ %08X has unexpected bytes (another mod?); photo mode disabled", static_cast<unsigned>(kPauseDecision));
    ok = false;
  }
  d3d::AddResetListener(&OnDeviceReset);
  if (!ok) {
    U2LOG("some hooks failed; photo mode disabled");
    hook::DisableAll();
    return false;
  }
  return hook::EnableAll();
}
}  // namespace photomode
