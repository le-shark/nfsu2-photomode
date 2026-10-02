#include "lens.h"
#include "rig.h"
#include "test.h"

using namespace photomode;
using u2::Vec3;

TEST(RigInitFromCameraKeepsDirection) {
  Rig r;
  Vec3 dir = u2::Normalize(Vec3{1, 2, -0.5f});
  r.InitFromCamera({10, 20, 3}, dir, {14, 25, 0});
  Vec3 f = r.Forward();
  CHECK_NEAR(f.x, dir.x, 1e-4);
  CHECK_NEAR(f.y, dir.y, 1e-4);
  CHECK_NEAR(f.z, dir.z, 1e-4);
}

TEST(RigOrbitLooksAtPivot) {
  Rig r;
  r.mode = RigMode::Orbit;
  r.yaw = 30;
  r.pitch = -10;
  r.orbitDist = 7;
  Vec3 car{100, -50, 2}, eye, target, up;
  r.Compute(car, &eye, &target, &up);
  Vec3 pivot = car + Vec3{0, 0, r.orbitHeight};
  CHECK_NEAR(u2::Length(pivot - eye), 7, 1e-3);
  Vec3 f = u2::Normalize(target - eye);
  Vec3 toPivot = u2::Normalize(pivot - eye);
  CHECK_NEAR(u2::Dot(f, toPivot), 1, 1e-4);
}

TEST(RigUpVectorAndRoll) {
  Rig r;
  r.mode = RigMode::Free;
  r.yaw = 45;
  r.pitch = 20;
  Vec3 eye, target, up;
  r.Compute({}, &eye, &target, &up);
  CHECK_NEAR(u2::Dot(up, u2::Normalize(target - eye)), 0, 1e-4);
  CHECK(up.z > 0.5f);  // level horizon, Z-up world
  r.roll = 90;
  r.Compute({}, &eye, &target, &up);
  CHECK_NEAR(u2::Length(up), 1, 1e-4);
  CHECK_NEAR(u2::Dot(up, u2::Normalize(target - eye)), 0, 1e-4);
  CHECK_NEAR(up.z, 0, 1e-3);  // rolled 90 degrees: up is horizontal
}

TEST(RigFreeClampsDistance) {
  Rig r;
  r.mode = RigMode::Free;
  r.maxDist = 30;
  r.moveSpeed = 100;
  r.eye = {0, 0, 0};
  RigInput in;
  in.moveForward = 1;
  for (int i = 0; i < 20; ++i) r.Update(in, 0.1f, {0, 0, 0});
  CHECK(u2::Length(r.eye) <= 30.001f);
}

TEST(RigOrbitZoomClamps) {
  Rig r;
  r.mode = RigMode::Orbit;
  r.orbitDist = 5;
  r.maxDist = 30;
  RigInput in;
  in.moveForward = 1;  // zoom in
  for (int i = 0; i < 100; ++i) r.Update(in, 0.1f, {});
  CHECK_NEAR(r.orbitDist, 1.0, 1e-4);
  in.moveForward = -1;
  for (int i = 0; i < 200; ++i) r.Update(in, 0.1f, {});
  CHECK_NEAR(r.orbitDist, 30.0, 1e-4);
}

TEST(RigModeSwitchKeepsView) {
  Rig r;
  r.mode = RigMode::Orbit;
  r.yaw = 120;
  r.pitch = -15;
  r.orbitDist = 8;
  Vec3 car{5, 5, 0}, e0, t0, u0, e1, t1, u1;
  r.Compute(car, &e0, &t0, &u0);
  r.SetMode(RigMode::Free, car);
  r.Compute(car, &e1, &t1, &u1);
  CHECK_NEAR(u2::Length(e1 - e0), 0, 1e-3);
  r.SetMode(RigMode::Orbit, car);
  r.Compute(car, &e1, &t1, &u1);
  CHECK_NEAR(u2::Length(e1 - e0), 0, 1e-3);
}

TEST(LensAperture) {
  CHECK_NEAR(lens::ApertureRadiusM(50, 2), 0.0125, 1e-6);
  CHECK_NEAR(lens::ApertureRadiusM(50, 0), 0, 1e-9);
}

TEST(LensDepthOfField) {
  float wide = lens::DepthOfFieldM(50, 1.4f, 5), narrow = lens::DepthOfFieldM(50, 16, 5);
  CHECK(wide > 0 && wide < narrow);
  CHECK(lens::DepthOfFieldM(200, 2.8f, 5) < lens::DepthOfFieldM(35, 2.8f, 5));
  // 50mm f/1.4 at 5 m, c = 0.03 mm: 4.62 m .. 5.45 m (standard DOF calculator)
  CHECK_NEAR(wide, 0.84, 0.01);
  CHECK(lens::DepthOfFieldM(24, 11, 50) >= 1e4f);  // beyond hyperfocal
}

TEST(LensTables) {
  CHECK(lens::Closest(lens::kFocals, 52) == 10);  // 50 mm
  CHECK(lens::Count(lens::kShutters) == lens::Count(lens::kShutterNames));
  CHECK_NEAR(lens::kFStops[lens::kDefaultFStop], 2.8, 1e-4);
  for (int i = 2; i < lens::Count(lens::kFStops); ++i) CHECK(lens::kFStops[i] > lens::kFStops[i - 1]);
  for (int i = 2; i < lens::Count(lens::kShutters); ++i) CHECK(lens::kShutters[i] > lens::kShutters[i - 1]);
}
