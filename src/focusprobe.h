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
// matrices (as Daylight's depth passes do: u2sdk/ao_math.h BuildFrame, verified-in-binary.md "Projection of view
// 1"). Daylight's own depth swap skips such frames (its target size check, daylight/depth.cpp Begin). Without INTZ or
// R32F the old box / ray method stays.
#include <cstdint>

#include "u2sdk/ao_math.h"
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
  int carTexels = 0;     // with countTexels: the car's texels within 2 cm of carZ
};

constexpr float kCarInflate = 0.10f;  // m around the car's box: wheels and parts just outside it, body roll
constexpr float kSky = 0.999999f;     // INTZ depth at or above this: nothing drawn

// The texels of a w x h image that the box (centre c, half sizes `half` along axes b; camera-relative) can cover: its 8
// corners through vpRel (row-vector, clip.w = view depth), plus one texel. The whole image when a corner is at or behind
// the camera. Returns false when the box is entirely off-screen.
inline bool ScreenBox(const u2::aomath::Frame& fr, const Vec3 b[3], const Vec3& c, const Vec3& half, int w, int h,
                      int* x0, int* y0, int* x1, int* y1) {
  const float* m = fr.vpRel;
  float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
  for (int i = 0; i < 8; ++i) {
    const Vec3 p = c + b[0] * (i & 1 ? half.x : -half.x) + b[1] * (i & 2 ? half.y : -half.y) +
                   b[2] * (i & 4 ? half.z : -half.z);
    const float cw = p.x * m[3] + p.y * m[7] + p.z * m[11] + m[15];
    if (cw < 0.05f) {  // at or behind the camera: no useful projection
      *x0 = *y0 = 0, *x1 = w, *y1 = h;
      return true;
    }
    const float u = ((p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12]) / cw + 1) * 0.5f;
    const float v = (1 - (p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13]) / cw) * 0.5f;
    u0 = u < u0 ? u : u0, u1 = u > u1 ? u : u1, v0 = v < v0 ? v : v0, v1 = v > v1 ? v : v1;
  }
  auto clampTo = [](float x, int n) { return x < 0 ? 0 : x > n ? n : static_cast<int>(x); };
  *x0 = clampTo(u0 * w - 1, w), *x1 = clampTo(u1 * w + 2, w);
  *y0 = clampTo(v0 * h - 1, h), *y1 = clampTo(v1 * h + 2, h);
  return *x0 < *x1 && *y0 < *y1;
}

// depth: w x h INTZ values (0..1), rows `pitch` floats apart; fr: view 1's camera-relative rays (ao_math.h);
// view z = p32 / (d - p22) (p22 just above 1, p32 < 0); car: the Focus Target, or null. countTexels: also count the
// car's texels within 2 cm of its nearest point (the Verbose log line only).
// Only the texels the car's box can cover are searched (ScreenBox; review 2026-10-22: the whole image took
// 2.5-3.6 ms at 1080p and 10-15 ms at 4K on the main thread).
inline Result Evaluate(const float* depth, int w, int h, int pitch, const u2::aomath::Frame& fr, float p22,
                       float p32, const Vec3& eye, const CarPose* car, bool countTexels = false) {
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
  int x0, y0, x1, y1;
  if (!ScreenBox(fr, b, centreRel, half, w, h, &x0, &y0, &x1, &y1)) return r;
  // The view depth of texel (x, y) when it shows part of the car, else a negative number.
  auto carDepth = [&](int x, int y) {
    const float d = depth[y * pitch + x];
    if (d >= kSky) return -1.0f;
    const float z = viewZ(d);
    const Vec3 rel = (tl + du * ((x + 0.5f) / w) + dv * ((y + 0.5f) / h)) * z;
    const Vec3 l = detail::ToLocal(b, rel - centreRel);
    return std::fabs(l.x) > half.x || std::fabs(l.y) > half.y || std::fabs(l.z) > half.z ? -1.0f : z;
  };
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const float d = depth[y * pitch + x];
      if (d >= kSky || (r.carHit && viewZ(d) >= r.carZ)) continue;  // cheap test before the box test
      const float z = carDepth(x, y);
      if (z < 0) continue;
      r.carHit = true;
      r.carZ = z;
    }
  if (r.carHit && countTexels)
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const float z = carDepth(x, y);
        if (z >= 0 && z < r.carZ + 0.02f) ++r.carTexels;
      }
  return r;
}

// The GPU side (focusprobe.cpp). Main thread only.
class FocusProbe {
 public:
  ~FocusProbe() { Free(); }
  // Half the screen size; false when the card can't (no INTZ, no R32F target): logged once, and the old method stays
  // for the rest of the game session.
  bool Ensure(IDirect3DDevice9* dev);
  bool Failed() const { return failed_; }
  // Around the game's Render3D call on the probe frame.
  void Begin(IDirect3DDevice9* dev) { super_.Begin(dev); }
  void End(IDirect3DDevice9* dev) { super_.End(dev); }
  // After End: depth -> CPU -> distances. car may be null; countTexels for the Verbose log.
  bool Read(IDirect3DDevice9* dev, const CarPose* car, bool countTexels, Result* out);
  void Free();  // the GPU objects (photo mode closes, device reset, before a 2x photo); a failure stays recorded

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
