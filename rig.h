#pragma once
// Photo-mode camera rig. Pure math (no game calls), so it's unit-tested on the host.
// World is Z-up. yaw: rotation about +Z (0 = +X), pitch: elevation, roll: about the view axis.
#include "u2sdk/u2math.h"

namespace photomode {

using u2::Vec3;

struct RigInput {
  float moveRight = 0, moveForward = 0, moveUp = 0;  // -1..1
  float lookYawDeg = 0, lookPitchDeg = 0;            // deltas this frame
  float rollDeg = 0;                                 // delta this frame
  float speedMul = 1;
};

enum class RigMode { Orbit = 0, Free = 1 };

inline constexpr float kMinOrbitDist = 1, kMaxPitch = 89, kMaxRoll = 90;  // metres, degrees

struct Rig {
  RigMode mode = RigMode::Orbit;
  Vec3 eye;              // free mode position
  float yaw = 0, pitch = 0, roll = 0;   // degrees
  float orbitDist = 6;
  float orbitHeight = 0.6f;             // pivot height above the car origin
  float moveSpeed = 6;                  // m/s
  float maxDist = 30;                   // clamp from anchor (car)
  // Optional box the eye stays inside (the garage set: photomode garage.h). In Orbit mode a clamped eye still looks at
  // the pivot.
  bool bounded = false;
  Vec3 boundMin, boundMax;

  Vec3 Bound(const Vec3& e) const {
    if (!bounded) return e;
    return {u2::Clamp(e.x, boundMin.x, boundMax.x), u2::Clamp(e.y, boundMin.y, boundMax.y),
            u2::Clamp(e.z, boundMin.z, boundMax.z)};
  }

  static Vec3 Forward(float yawDeg, float pitchDeg) {
    float y = yawDeg * u2::kDegToRad, p = pitchDeg * u2::kDegToRad;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
  }
  Vec3 Forward() const { return Forward(yaw, pitch); }
  Vec3 Pivot(const Vec3& anchor) const { return anchor + Vec3{0, 0, orbitHeight}; }

  // Yaw/pitch looking along `dir` (pitch kept off the poles, where the up vector is undefined).
  void AimAt(const Vec3& dir) {
    Vec3 d = u2::Normalize(dir);
    yaw = std::atan2(d.y, d.x) * u2::kRadToDeg;
    pitch = u2::Clamp(std::asin(u2::Clamp(d.z, -1.f, 1.f)) * u2::kRadToDeg, -kMaxPitch, kMaxPitch);
  }
  void SetOrbitDist(float d) { orbitDist = u2::Clamp(d, kMinOrbitDist, maxDist); }

  // Start from the game camera's position/direction, anchored on the car.
  void InitFromCamera(const Vec3& camPos, const Vec3& camDir, const Vec3& anchor) {
    AimAt(camDir);
    roll = 0;
    eye = camPos;
    SetOrbitDist(u2::Length(camPos - Pivot(anchor)));
  }

  void SetMode(RigMode m, const Vec3& anchor) {
    if (m == mode) return;
    Vec3 e, t, u;
    Compute(anchor, &e, &t, &u);
    mode = m;
    if (m == RigMode::Free) {
      eye = e;
    } else {  // keep looking at the car from the current spot
      AimAt(Pivot(anchor) - e);
      SetOrbitDist(u2::Length(Pivot(anchor) - e));
    }
  }

  // Continue from an arbitrary camera pose (e.g. where a shutter-follow exposure ended). Free mode
  // takes it as is; orbit mode keeps the position and roll and aims at the pivot again.
  void SetPose(const Vec3& camEye, const Vec3& camFwd, const Vec3& camUp, const Vec3& anchor) {
    Vec3 f = u2::Normalize(camFwd);
    Vec3 r = u2::Normalize(u2::Cross(f, Vec3{0, 0, 1}));
    if (u2::Length(r) > 0.5f) {
      Vec3 up0 = u2::Cross(r, f);  // unrolled up, as in Compute
      roll = u2::Clamp(std::atan2(u2::Dot(u2::Cross(f, up0), camUp), u2::Dot(up0, camUp)) * u2::kRadToDeg, -kMaxRoll, kMaxRoll);
    }
    if (mode == RigMode::Orbit) {
      f = Pivot(anchor) - camEye;
      SetOrbitDist(u2::Length(f));
    }
    AimAt(f);
    eye = camEye;
  }

  void Update(const RigInput& in, float dt, const Vec3& anchor) {
    yaw += in.lookYawDeg;
    pitch = u2::Clamp(pitch + in.lookPitchDeg, -kMaxPitch, kMaxPitch);
    roll = u2::Clamp(roll + in.rollDeg, -kMaxRoll, kMaxRoll);
    float step = moveSpeed * in.speedMul * dt;
    if (mode == RigMode::Free) {
      Vec3 f = Forward();
      Vec3 r = u2::Normalize(u2::Cross(f, Vec3{0, 0, 1}));
      eye += f * (in.moveForward * step) + r * (in.moveRight * step) + Vec3{0, 0, in.moveUp * step};
      Vec3 off = eye - anchor;
      float l = u2::Length(off);
      if (l > maxDist) eye = anchor + off * (maxDist / l);
      eye = Bound(eye);
    } else {
      SetOrbitDist(orbitDist - in.moveForward * step);
      // strafing circles the car: arc length `step` at radius orbitDist
      yaw -= in.moveRight * step / orbitDist * u2::kRadToDeg;
      orbitHeight = u2::Clamp(orbitHeight + in.moveUp * step, -2.f, maxDist);
    }
  }

  void Compute(const Vec3& anchor, Vec3* outEye, Vec3* outTarget, Vec3* outUp) const {
    Vec3 f = Forward();
    if (mode == RigMode::Free) {
      *outEye = Bound(eye);
    } else {
      const Vec3 e = Pivot(anchor) - f * orbitDist;
      *outEye = Bound(e);
      const Vec3 d = Pivot(anchor) - *outEye;
      if (u2::Length(*outEye - e) > 1e-4f && u2::Length(d) > 1e-3f) f = u2::Normalize(d);
    }
    *outTarget = *outEye + f;
    Vec3 r = u2::Normalize(u2::Cross(f, Vec3{0, 0, 1}));
    Vec3 up = u2::Cross(r, f);
    *outUp = u2::RotateAxis(up, f, roll * u2::kDegToRad);
  }
};

}  // namespace photomode
