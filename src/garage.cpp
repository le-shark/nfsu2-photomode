#include "garage.h"

#include <cstdint>

#include "u2sdk/game.h"
#include "u2sdk/log.h"

namespace photomode::garage {
namespace {
using u2::Var;
using u2::Vec3;

constexpr int kFlowFrontEnd = 3;  // [0x8654A4] = 3 at the FE load's end (0x5800BA)
constexpr const char* kGaragePkg = "GarageMain.fng";
// FindFNGObject 0x51BD10 cdecl(name): the package's FNG object (the career hub finds GarageMain so, 0x4AF451..0x4AF45B).
auto FindFngObject = reinterpret_cast<uint8_t*(__cdecl*)(const char*)>(0x51BD10);
auto IsFngLoaded = reinterpret_cast<bool(__cdecl*)(const char*)>(0x52CF60);
// GarageMain: +0x1C8 non-zero = the set is not drawn (RenderPlatform 0x4A733D returns; messages 0x0AD4BBDC /
// 0x18883F75 set and clear it, 0x4D8D20), +0x88 the BACKDROP model, submitted with an identity matrix and a zero
// translation (0x4A73A7..0x4A741A; [0x78E004..0x78E00C] = 0). eModel +0xC eSolid*, eSolid +0x20 / +0x30 its box
// (verified-in-binary.md "eSolid", "A world solid end to end").
constexpr uintptr_t kHidden = 0x1C8, kBackdrop = 0x88, kModelSolid = 0xC, kSolidMin = 0x20, kSolidMax = 0x30;
constexpr uintptr_t kFloorHeight = 0x7F45A8;  // float 0.15, the platform floor (PlaceCar 0x4A66C2)
// GarageMain's tick steps the emitter system 0x8357E0 with real time [0x865198] (0x4D8A3C..0x4D8A48), so the freeze
// does not hold the Showroom's smoke.
constexpr uintptr_t kEmitterSystem = 0x8357E0;
constexpr float kWallMargin = 0.3f, kFloorMargin = 0.1f;
// GarageMain's car orbit controller (2026-11-03, investigation agent, key bytes re-read): +0x240 its enable flag,
// written 1 only by GarageMain's ctor (0x4EAF40); the CARSEL_ORBIT_* joy handler 0x4A5740 returns while it is 0
// (0x4A5743..0x4A5748) and otherwise writes the rates +0x248 (turntable spin), +0x24C (pitch), +0x250 (zoom). The
// tick spins the disc by +0x248 with real time (0x4D8A60..0x4D8A7A, angle +0x274), so the freeze does not hold it. A
// rate latched by a key held at opening has its release dropped while the flag is 0, so the rates are cleared too.
constexpr uintptr_t kOrbitEnabled = 0x240, kOrbitRates[3] = {0x248, 0x24C, 0x250};

uint8_t* Garage() { return IsFngLoaded(kGaragePkg) ? FindFngObject(kGaragePkg) : nullptr; }

uint8_t* g_heldGarage = nullptr;  // the GarageMain whose orbit input is off (HoldTurntable)
int g_savedOrbitEnabled = 0;

void ClearOrbitRates(uint8_t* gm) {
  for (uintptr_t r : kOrbitRates) *reinterpret_cast<float*>(gm + r) = 0;
}
}  // namespace

bool Present() {
  if (u2::GameFlowState() != kFlowFrontEnd) return false;
  uint8_t* g = Garage();
  return g && *reinterpret_cast<int*>(g + kHidden) == 0;
}

bool CameraBox(Vec3* mn, Vec3* mx) {
  uint8_t* g = Garage();
  if (!g) return false;
  auto solid = *reinterpret_cast<uint8_t**>(g + kBackdrop + kModelSolid);
  if (!solid) return false;
  auto a = reinterpret_cast<const float*>(solid + kSolidMin), b = reinterpret_cast<const float*>(solid + kSolidMax);
  for (int i = 0; i < 3; ++i)
    if (!(b[i] - a[i] > 2 && b[i] - a[i] < 5000)) return false;  // empty or not a box
  const float floor = Var<float>(kFloorHeight);
  *mn = {a[0] + kWallMargin, a[1] + kWallMargin, (a[2] > floor ? a[2] : floor) + kFloorMargin};
  *mx = {b[0] - kWallMargin, b[1] - kWallMargin, b[2] - kWallMargin};
  return mx->z > mn->z;
}

bool IsSetEmitters(const void* particleManager) {
  return reinterpret_cast<uintptr_t>(particleManager) == kEmitterSystem;
}

void HoldTurntable(bool hold) {
  uint8_t* gm = Garage();
  if (hold) {
    if (!gm || g_heldGarage) return;
    g_heldGarage = gm;
    g_savedOrbitEnabled = *reinterpret_cast<int*>(gm + kOrbitEnabled);
    *reinterpret_cast<int*>(gm + kOrbitEnabled) = 0;
    ClearOrbitRates(gm);
    U2LOG("garage: turntable input off (was %d)", g_savedOrbitEnabled);
    return;
  }
  // Only the same GarageMain gets its switch back: a garage left meanwhile is gone (a new one starts enabled).
  if (gm && gm == g_heldGarage) {
    ClearOrbitRates(gm);
    *reinterpret_cast<int*>(gm + kOrbitEnabled) = g_savedOrbitEnabled;
    U2LOG("garage: turntable input back");
  }
  g_heldGarage = nullptr;
}

}  // namespace photomode::garage
