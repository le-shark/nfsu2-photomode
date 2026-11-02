#include "garage.h"

#include <cstdint>

#include "u2sdk/game.h"

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

uint8_t* Garage() { return IsFngLoaded(kGaragePkg) ? FindFngObject(kGaragePkg) : nullptr; }
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

}  // namespace photomode::garage
