#pragma once
// How the camera follows the target car while the shutter is open, and car-based autofocus.
// Pure math (no game calls), so it's unit-tested on the host. World is Z-up.
#include <cmath>

#include "u2sdk/u2math.h"

namespace photomode {

using u2::Vec3;

enum class Follow {
  Off,    // camera stays still
  Pan,    // camera stays in place and swivels to keep the car in frame (roadside panning)
  Track,  // camera moves with the car's position, keeps its own rotation (camera car alongside)
  Rig,    // camera moves and rotates with the car, as if bolted to it (rig shot)
  Count
};
inline const char* FollowName(Follow f) {
  static const char* n[static_cast<int>(Follow::Count)] = {"Off", "Pan", "Track", "Rig"};
  return n[static_cast<int>(f)];
}

struct CarPose {
  Vec3 pos;
  Vec3 axis[3];  // the car's orientation rows (any consistent basis; orthonormalised here)
};

struct CamPose {
  Vec3 eye, fwd, up;
};

namespace detail {
// Orthonormal basis from the (possibly scaled / slightly skewed) car rows.
inline void Basis(const CarPose& c, Vec3 b[3]) {
  b[0] = u2::Normalize(c.axis[0]);
  b[2] = u2::Normalize(c.axis[2] - b[0] * u2::Dot(c.axis[2], b[0]));
  b[1] = u2::Cross(b[2], b[0]);
}
inline Vec3 ToLocal(const Vec3 b[3], const Vec3& v) { return {u2::Dot(v, b[0]), u2::Dot(v, b[1]), u2::Dot(v, b[2])}; }
inline Vec3 ToWorld(const Vec3 b[3], const Vec3& l) { return b[0] * l.x + b[1] * l.y + b[2] * l.z; }
}  // namespace detail

// Camera pose for a sample, given the pose when the shutter opened (start, car0) and the car now.
inline CamPose FollowPose(Follow mode, const CamPose& start, const CarPose& car0, const CarPose& car1) {
  CamPose p = start;
  switch (mode) {
    case Follow::Track: p.eye = start.eye + (car1.pos - car0.pos); break;
    case Follow::Rig: {
      Vec3 b0[3], b1[3];
      detail::Basis(car0, b0);
      detail::Basis(car1, b1);
      p.eye = car1.pos + detail::ToWorld(b1, detail::ToLocal(b0, start.eye - car0.pos));
      p.fwd = detail::ToWorld(b1, detail::ToLocal(b0, start.fwd));
      p.up = detail::ToWorld(b1, detail::ToLocal(b0, start.up));
      break;
    }
    case Follow::Pan: {
      Vec3 d0 = u2::Normalize(car0.pos - start.eye), d1 = u2::Normalize(car1.pos - start.eye);
      if (u2::Length(d0) < 0.5f || u2::Length(d1) < 0.5f) break;
      float dYaw = std::atan2(d1.y, d1.x) - std::atan2(d0.y, d0.x);
      auto elev = [](const Vec3& v) { return std::asin(u2::Clamp(u2::Normalize(v).z, -1.f, 1.f)); };
      // Keep the view off the poles: past them the image would turn upside down.
      const float lim = 89 * u2::kDegToRad, p0 = elev(start.fwd);
      float dPitch = u2::Clamp(elev(d1) - elev(d0), -lim - p0, lim - p0);
      const Vec3 z{0, 0, 1};
      p.fwd = u2::RotateAxis(start.fwd, z, dYaw);
      p.up = u2::RotateAxis(start.up, z, dYaw);
      Vec3 right = u2::Normalize(u2::Cross(p.fwd, z));  // positive angle about this raises the view
      if (u2::Length(right) > 0.5f) {
        p.fwd = u2::RotateAxis(p.fwd, right, dPitch);
        p.up = u2::RotateAxis(p.up, right, dPitch);
      }
      break;
    }
    default: break;
  }
  return p;
}

// Autofocus uses an approximate car box, 4.4 x 1.8 x 1.3 m, centred kCarCentreUp above the
// car origin along its up row.
inline constexpr float kCarHalfLen = 2.2f, kCarHalfWid = 0.9f, kCarHalfHgt = 0.65f, kCarCentreUp = 0.5f;

// Distance along the view direction to the car's centre (centre autofocus).
inline float CarCentreDepth(const CamPose& cam, const CarPose& car) {
  Vec3 b[3];
  detail::Basis(car, b);
  return u2::Dot(car.pos + b[2] * kCarCentreUp - cam.eye, u2::Normalize(cam.fwd));
}

// Distance along the view direction to the nearest corner of the car box (closest autofocus).
inline float NearestCarDepth(const CamPose& cam, const CarPose& car) {
  Vec3 b[3];
  detail::Basis(car, b);
  Vec3 c = car.pos + b[2] * kCarCentreUp;
  Vec3 f = u2::Normalize(cam.fwd);
  float best = 1e9f;
  for (int i = 0; i < 8; ++i) {
    Vec3 k = c + b[0] * ((i & 1) ? kCarHalfLen : -kCarHalfLen) + b[1] * ((i & 2) ? kCarHalfWid : -kCarHalfWid) +
             b[2] * ((i & 4) ? kCarHalfHgt : -kCarHalfHgt);
    float d = u2::Dot(k - cam.eye, f);
    if (d < best) best = d;
  }
  return best;
}

}  // namespace photomode
