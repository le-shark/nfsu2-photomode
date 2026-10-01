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

struct Rig {
  RigMode mode = RigMode::Orbit;
  Vec3 eye;              // free mode position
  float yaw = 0, pitch = 0, roll = 0;   // degrees
  float orbitDist = 6;
  float orbitHeight = 0.6f;             // pivot height above the car origin
  float moveSpeed = 6;                  // m/s
  float maxDist = 30;                   // clamp from anchor (car)

  static Vec3 Forward(float yawDeg, float pitchDeg) {
    float y = yawDeg * u2::kDegToRad, p = pitchDeg * u2::kDegToRad;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
  }
  Vec3 Forward() const { return Forward(yaw, pitch); }

  // Start from the game camera's position/direction, anchored on the car.
  void InitFromCamera(const Vec3& camPos, const Vec3& camDir, const Vec3& anchor) {
    Vec3 d = u2::Normalize(camDir);
    yaw = std::atan2(d.y, d.x) * u2::kRadToDeg;
    pitch = std::asin(u2::Clamp(d.z, -1.f, 1.f)) * u2::kRadToDeg;
    roll = 0;
    eye = camPos;
    Vec3 pivot = anchor + Vec3{0, 0, orbitHeight};
    orbitDist = u2::Clamp(u2::Length(camPos - pivot), 1.0f, maxDist);
  }

  void SetMode(RigMode m, const Vec3& anchor) {
    if (m == mode) return;
    Vec3 e, t, u;
    Compute(anchor, &e, &t, &u);
    mode = m;
    if (m == RigMode::Free) {
      eye = e;
    } else {
      // Keep looking at the car from the current spot.
      Vec3 pivot = anchor + Vec3{0, 0, orbitHeight};
      Vec3 d = u2::Normalize(pivot - e);
      yaw = std::atan2(d.y, d.x) * u2::kRadToDeg;
      pitch = std::asin(u2::Clamp(d.z, -1.f, 1.f)) * u2::kRadToDeg;
      orbitDist = u2::Clamp(u2::Length(pivot - e), 1.0f, maxDist);
    }
  }

  void Update(const RigInput& in, float dt, const Vec3& anchor) {
    yaw += in.lookYawDeg;
    pitch = u2::Clamp(pitch + in.lookPitchDeg, -89.f, 89.f);
    roll = u2::Clamp(roll + in.rollDeg, -90.f, 90.f);
    float step = moveSpeed * in.speedMul * dt;
    if (mode == RigMode::Free) {
      Vec3 f = Forward();
      Vec3 r = u2::Normalize(u2::Cross(f, Vec3{0, 0, 1}));
      eye += f * (in.moveForward * step) + r * (in.moveRight * step) + Vec3{0, 0, in.moveUp * step};
      Vec3 off = eye - anchor;
      float l = u2::Length(off);
      if (l > maxDist) eye = anchor + off * (maxDist / l);
    } else {
      orbitDist = u2::Clamp(orbitDist - in.moveForward * step, 1.0f, maxDist);
      // strafing circles the car: arc length `step` at radius orbitDist
      yaw -= in.moveRight * step / orbitDist * u2::kRadToDeg;
      orbitHeight = u2::Clamp(orbitHeight + in.moveUp * step, -2.f, maxDist);
    }
  }

  void Compute(const Vec3& anchor, Vec3* outEye, Vec3* outTarget, Vec3* outUp) const {
    Vec3 f = Forward();
    if (mode == RigMode::Free) {
      *outEye = eye;
    } else {
      Vec3 pivot = anchor + Vec3{0, 0, orbitHeight};
      *outEye = pivot - f * orbitDist;
    }
    *outTarget = *outEye + f;
    Vec3 r = u2::Normalize(u2::Cross(f, Vec3{0, 0, 1}));
    Vec3 up = u2::Cross(r, f);
    *outUp = u2::RotateAxis(up, f, roll * u2::kDegToRad);
  }
};

}  // namespace photomode
