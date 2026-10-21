#pragma once
// Precise autofocus (user 2026-10-21: Aim and Closest "don't feel precise", the focused point soft at wide apertures).
// Before, both used an approximate box per car (Closest: its nearest corner, in the air in front of the body) and the
// collision ray for the world. Now, once the camera has stopped, one probe frame renders the scene's depth and the
// distances come from what is actually drawn:
// - Aim: the view depth of the pixels at the screen centre (car or world; sky = no hit).
// - Closest: the nearest drawn point inside the Focus Target car's box (slightly enlarged): its real body, wheels and
//   add-on parts, whatever is visible.
// How (agent research 2026-10-21, status doc "Beta feedback"): for one Render3D call the 3D pass renders into a
// half-size colour target and an INTZ depth texture, with the 2x photo's swap (supersample.cpp, [game] 2026-10-04);
// the screen shows the frame held just before (Accum::Hold / ShowHold), so nothing blinks. A shader copies the depth
// into an R32F target, one readback brings it to the CPU, and Evaluate below turns it into distances with view 1's
// matrices (as Daylight's depth passes do: daylight/ao_math.h BuildFrame, verified-in-binary.md "Projection of view
// 1"). Daylight's own depth swap skips such frames (its target size check, daylight/depth.cpp Begin). Without INTZ or
// R32F the old box / ray method stays.
#include <cstdint>

#include "../daylight/ao_math.h"
#include "follow.h"
#include "supersample.h"

struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;
struct IDirect3DPixelShader9;
struct IDirect3DStateBlock9;

namespace photomode::probe {

struct Result {
  bool aimHit = false;   // something is drawn at the screen centre
  float aimZ = 0;        // its view depth, m
  bool carHit = false;   // part of the Focus Target car is visible
  float carZ = 0;        // the view depth of its nearest visible point, m
  int carTexels = 0;
};

constexpr float kCarInflate = 0.10f;  // m around the car's box: wheels and parts just outside it, body roll
constexpr float kSky = 0.999999f;     // INTZ depth at or above this: nothing drawn

// depth: w x h INTZ values (0..1), rows `pitch` floats apart; fr: view 1's camera-relative rays (ao_math.h);
// view z = p32 / (d - p22) (p22 just above 1, p32 < 0); car: the Focus Target, or null.
inline Result Evaluate(const float* depth, int w, int h, int pitch, const daylight::aomath::Frame& fr, float p22,
                       float p32, const Vec3& eye, const CarPose* car) {
  Result r;
  auto viewZ = [&](float d) { return p32 / (d - p22); };
  // Aim: the 2 x 2 texels around the centre; the nearest one, so an edge between car and background picks the car.
  for (int y = h / 2 - 1; y <= h / 2; ++y)
    for (int x = w / 2 - 1; x <= w / 2; ++x) {
      if (x < 0 || y < 0) continue;
      const float d = depth[y * pitch + x];
      if (d >= kSky) continue;
      const float z = viewZ(d);
      if (!r.aimHit || z < r.aimZ) r.aimHit = true, r.aimZ = z;
    }
  if (!car) return r;
  Vec3 b[3];
  detail::Basis(*car, b);
  // Everything relative to the eye, so the numbers stay small for floats.
  const Vec3 centreRel = car->pos + detail::ToWorld(b, detail::BoxCentre(*car)) - eye;
  const Vec3 half = detail::BoxHalf(*car) + Vec3{kCarInflate, kCarInflate, kCarInflate};
  const Vec3 tl{fr.rayTL[0], fr.rayTL[1], fr.rayTL[2]}, du{fr.rayDU[0], fr.rayDU[1], fr.rayDU[2]},
      dv{fr.rayDV[0], fr.rayDV[1], fr.rayDV[2]};
  for (int y = 0; y < h; ++y) {
    const float v = (y + 0.5f) / h;
    const float* row = depth + y * pitch;
    for (int x = 0; x < w; ++x) {
      const float d = row[x];
      if (d >= kSky) continue;
      const float z = viewZ(d);
      if (r.carHit && z >= r.carZ) continue;
      const float u = (x + 0.5f) / w;
      const Vec3 rel = (tl + du * u + dv * v) * z;
      const Vec3 l = detail::ToLocal(b, rel - centreRel);
      if (std::fabs(l.x) > half.x || std::fabs(l.y) > half.y || std::fabs(l.z) > half.z) continue;
      r.carHit = true;
      r.carZ = z;
    }
  }
  if (r.carHit)  // count after the fact (for the log): texels of the car within 2 cm of its nearest point
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const float d = depth[y * pitch + x];
        if (d < kSky && viewZ(d) < r.carZ + 0.02f) ++r.carTexels;
      }
  return r;
}

// The GPU side (focusprobe.cpp). Main thread only.
class FocusProbe {
 public:
  ~FocusProbe() { Free(); }
  // Half the screen size; false (logged once) when the card can't (no INTZ, no R32F target): the old method stays.
  bool Ensure(IDirect3DDevice9* dev);
  bool Failed() const { return failed_; }
  // Around the game's Render3D call on the probe frame.
  void Begin(IDirect3DDevice9* dev) { super_.Begin(dev); }
  void End(IDirect3DDevice9* dev) { super_.End(dev); }
  // After End: depth -> CPU -> distances. car may be null.
  bool Read(IDirect3DDevice9* dev, const CarPose* car, Result* out);
  void Free();  // photo mode closes, device reset

 private:
  void Fail(const char* what, long hr);
  Supersample super_;
  IDirect3DTexture9* r32_ = nullptr;      // depth copied as floats
  IDirect3DSurface9* sys_ = nullptr;      // its SYSTEMMEM copy
  IDirect3DPixelShader9* ps_ = nullptr;
  IDirect3DStateBlock9* state_ = nullptr;
  int w_ = 0, h_ = 0;
  bool failed_ = false;
};

}  // namespace photomode::probe
