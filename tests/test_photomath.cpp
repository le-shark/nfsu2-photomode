#include <string>

#include "follow.h"
#include "lens.h"
#include "rig.h"
#include "sampling.h"
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

TEST(LensTables) {
  CHECK(lens::Closest(lens::kFocals, 52) == 10);  // 50 mm
  CHECK(lens::Count(lens::kShutters) == lens::Count(lens::kShutterNames));
  CHECK_NEAR(lens::kFStops[lens::kDefaultFStop], 2.8, 1e-4);
  for (int i = 2; i < lens::Count(lens::kFStops); ++i) CHECK(lens::kFStops[i] > lens::kFStops[i - 1]);
  for (int i = 2; i < lens::Count(lens::kShutters); ++i) CHECK(lens::kShutters[i] > lens::kShutters[i - 1]);
}

// ---- shutter follow / car autofocus (follow.h) ----
namespace {
CarPose CarAt(Vec3 pos, float yawDeg) {
  float y = yawDeg * u2::kDegToRad;
  return {pos, {{std::cos(y), std::sin(y), 0}, {-std::sin(y), std::cos(y), 0}, {0, 0, 1}}};
}
bool Near(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return u2::Length(a - b) < eps; }
}  // namespace

TEST(FollowOffAndTrack) {
  CamPose s{{0, -5, 1}, {0, 1, 0}, {0, 0, 1}};
  CarPose c0 = CarAt({0, 0, 0}, 0), c1 = CarAt({3, 1, 0}, 30);
  CamPose off = FollowPose(Follow::Off, s, c0, c1);
  CHECK(Near(off.eye, s.eye) && Near(off.fwd, s.fwd));
  CamPose tr = FollowPose(Follow::Track, s, c0, c1);
  CHECK(Near(tr.eye, {3, -4, 1}) && Near(tr.fwd, s.fwd));
}

TEST(FollowRigKeepsCarRelativePose) {
  // Camera 5 m to the car's right, looking at it; the car drives 10 m and turns 90 degrees left.
  CamPose s{{0, -5, 1}, {0, 1, 0}, {0, 0, 1}};
  CarPose c0 = CarAt({0, 0, 0}, 0), c1 = CarAt({10, 10, 0}, 90);
  CamPose p = FollowPose(Follow::Rig, s, c0, c1);
  CHECK(Near(p.eye, {15, 10, 1}));  // still on the car's right side
  CHECK(Near(p.fwd, {-1, 0, 0}));   // still looking at the car
  CHECK(Near(p.up, {0, 0, 1}));
}

TEST(FollowPanSwivelsInPlace) {
  CamPose s{{0, -10, 0}, {0, 1, 0}, {0, 0, 1}};
  CarPose c0 = CarAt({0, 0, 0}, 0), c1 = CarAt({10, 0, 0}, 0);
  CamPose p = FollowPose(Follow::Pan, s, c0, c1);
  CHECK(Near(p.eye, s.eye));
  CHECK(Near(p.fwd, u2::Normalize(Vec3{10, 10, 0})));  // aimed at the car again
  CHECK(Near(p.up, {0, 0, 1}));
  // Car rising: the view tilts up and stays level.
  CamPose q = FollowPose(Follow::Pan, s, c0, CarAt({0, 0, 10}, 0));
  CHECK(Near(q.fwd, u2::Normalize(Vec3{0, 10, 10})));
}

TEST(NearestCarDepth) {
  // Looking along +Y at a car 10 m ahead, nose pointing at the camera (-Y): the front corners are
  // 2.2 m closer than the centre.
  CamPose cam{{0, -10, 0.5f}, {0, 1, 0}, {0, 0, 1}};
  CHECK_NEAR(NearestCarDepth(cam, CarAt({0, 0, 0}, -90)), 7.8, 1e-3);
  // Side-on: the near side is half a width closer.
  CHECK_NEAR(NearestCarDepth(cam, CarAt({0, 0, 0}, 0)), 9.1, 1e-3);
}

TEST(RigSetPoseRoundTrip) {
  Rig r;
  r.mode = RigMode::Free;
  r.eye = {1, 2, 3};
  r.yaw = 40;
  r.pitch = -10;
  r.roll = 15;
  Vec3 e, t, u;
  r.Compute({}, &e, &t, &u);
  Rig q;
  q.mode = RigMode::Free;
  q.SetPose(e, t - e, u, {});
  CHECK_NEAR(q.yaw, 40, 1e-2);
  CHECK_NEAR(q.pitch, -10, 1e-2);
  CHECK_NEAR(q.roll, 15, 1e-2);
  CHECK(Near(q.eye, e));
  // Orbit: keeps the position and looks at the pivot.
  Rig o;
  o.mode = RigMode::Orbit;
  o.SetPose({0, -10, 0.6f}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0});
  o.Compute({0, 0, 0}, &e, &t, &u);
  CHECK(Near(e, {0, -10, 0.6f}));
  CHECK(Near(u2::Normalize(t - e), {0, 1, 0}));
}

TEST(RigAimAtStaysOffThePoles) {
  Rig r;
  r.mode = RigMode::Free;
  r.InitFromCamera({}, {0, 0, -1}, {});  // straight down
  CHECK_NEAR(r.pitch, -kMaxPitch, 1e-3);
  Vec3 e, t, u;
  r.Compute({}, &e, &t, &u);
  CHECK_NEAR(u2::Length(u), 1, 1e-3);  // up vector still defined
}

TEST(RigOrbitStrafeKeepsDistance) {
  Rig r;
  r.mode = RigMode::Orbit;
  r.orbitDist = 8;
  RigInput in;
  in.moveRight = 1;
  Vec3 e0, e1, t, u;
  r.Compute({}, &e0, &t, &u);
  r.Update(in, 0.1f, {});
  r.Compute({}, &e1, &t, &u);
  Vec3 pivot = r.Pivot({});
  CHECK_NEAR(u2::Length(e1 - pivot), 8, 1e-3);
  CHECK_NEAR(u2::Length(e1 - e0), r.moveSpeed * 0.1f, 0.01);  // arc length ~ chord for a short step
}

TEST(FollowPanStaysOffThePoles) {
  // Camera already looking up 80 degrees; the car climbs further: the view stops short of the zenith.
  float p = 80 * u2::kDegToRad;
  CamPose s{{0, 0, 0}, {0, std::cos(p), std::sin(p)}, {0, -std::sin(p), std::cos(p)}};
  CamPose q = FollowPose(Follow::Pan, s, CarAt({0, 10, 0}, 0), CarAt({0, 1, 10}, 0));
  CHECK(q.fwd.y > 0);    // not flipped over the top
  CHECK(q.up.z > 0);     // image not upside down
  CHECK_NEAR(std::asin(q.fwd.z) * u2::kRadToDeg, 89, 0.05);
}

TEST(FollowPanCarAtEyeIsStill) {
  CamPose s{{0, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  CamPose q = FollowPose(Follow::Pan, s, CarAt({0, 0, 0}, 0), CarAt({5, 0, 0}, 0));
  CHECK(Near(q.fwd, s.fwd) && Near(q.up, s.up));
}

TEST(FollowRigWithRolledCar) {
  // Car rolls 90 degrees onto its left side: a camera above it ends up on its left.
  CamPose s{{0, 0, 5}, {0, 0, -1}, {1, 0, 0}};
  CarPose c0 = CarAt({}, 0), c1{{}, {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};
  CamPose p = FollowPose(Follow::Rig, s, c0, c1);
  CHECK(Near(p.eye, {0, -5, 0}));
  CHECK(Near(p.fwd, {0, 1, 0}));
  CHECK(Near(p.up, {1, 0, 0}));
}

TEST(CarCentreDepth) {
  CamPose cam{{0, -10, 0.5f}, {0, 1, 0}, {0, 0, 1}};
  CHECK_NEAR(CarCentreDepth(cam, CarAt({0, 0, 0}, 0)), 10, 1e-3);
  CHECK(NearestCarDepth(cam, CarAt({0, 0, 0}, 0)) < CarCentreDepth(cam, CarAt({0, 0, 0}, 0)));
}

TEST(RayCarBoxHit) {
  CarPose car = CarAt({0, 0, 0}, 0);  // box centre (0, 0, 0.5), half 2.2 x 0.9 x 0.65
  // Side on, from 10 m to the left: enters at y = 0.9.
  CHECK_NEAR(RayCarBoxHit({0, 10, 0.5f}, {0, -1, 0}, car, 500), 9.1, 1e-3);
  // Head on along the car: enters at x = 2.2.
  CHECK_NEAR(RayCarBoxHit({20, 0, 0.5f}, {-1, 0, 0}, car, 500), 17.8, 1e-3);
  // Rotated car (90 deg): its length now runs along y.
  CHECK_NEAR(RayCarBoxHit({0, 10, 0.5f}, {0, -1, 0}, CarAt({0, 0, 0}, 90), 500), 7.8, 1e-3);
  // Misses: passes over the roof, points away, beyond maxT, eye inside the box.
  CHECK(RayCarBoxHit({0, 10, 2}, {0, -1, 0}, car, 500) < 0);
  CHECK(RayCarBoxHit({0, 10, 0.5f}, {0, 1, 0}, car, 500) < 0);
  CHECK(RayCarBoxHit({0, 10, 0.5f}, {0, -1, 0}, car, 5) < 0);
  CHECK(RayCarBoxHit({0, 0, 0.5f}, {0, -1, 0}, car, 500) < 0);
}

TEST(SampleAdjustMatchesCaptureMath) {
  SampleParams p{128, 1, 0.01f, 5, true};
  u2::ProjAdjust a = SampleAdjust(p, 7, 0, 1920, 1080);
  float x, y;
  u2::GoldenDisc(7, 128, &x, &y);
  CHECK_NEAR(a.lensX, x * 0.01f, 1e-7);
  CHECK_NEAR(a.lensY, y * 0.01f, 1e-7);
  CHECK_NEAR(a.jitterX, (u2::Halton(8, 2) - 0.5f) * 2 / 1920, 1e-9);
  CHECK_NEAR(a.jitterY, (u2::Halton(8, 3) - 0.5f) * 2 / 1080, 1e-9);
  CHECK_NEAR(a.focusDist, 5, 1e-6);
  CHECK(a.tiles == 1);
  p.tiles = 2;
  a = SampleAdjust(p, 7, 3, 1920, 1080);
  CHECK(a.tiles == 2 && a.tx == 1 && a.ty == 1);
  CHECK_NEAR(a.jitterX, (u2::Halton(8, 2) - 0.5f) * 2 / (1920 * 2), 1e-9);
  SampleParams pin{128, 1, 0, 5, false};
  a = SampleAdjust(pin, 7, 0, 1920, 1080);
  CHECK(a.lensX == 0 && a.lensY == 0 && a.jitterX == 0 && a.jitterY == 0);
}

TEST(SampleAdjustClampsFocus) {
  SampleParams p{32, 1, 0.01f, 0.01f, true};
  CHECK_NEAR(SampleAdjust(p, 0, 0, 640, 480).focusDist, 0.05f, 1e-7);
}

TEST(SampleKeyEquality) {
  SampleKey a{{1, 2, 3}, {0, 1, 0}, {0, 0, 1}, 9000, {128, 1, 0.01f, 5, true}, 0};
  SampleKey b = a;
  CHECK(a == b);
  b = a; b.eye.x += 0.001f; CHECK(!(a == b));
  b = a; b.fov += 1; CHECK(!(a == b));
  b = a; b.p.focusDist = 6; CHECK(!(a == b));
  b = a; b.p.samples = 32; CHECK(!(a == b));
  b = a; b.p.tiles = 2; CHECK(!(a == b));
  b = a; b.shutterSec = 0.03f; CHECK(!(a == b));
}

TEST(RunningMeanIsExactMean) {
  const float xs[] = {0.1f, 0.5f, 0.9f, 0.3f};
  float m = 0;
  for (int k = 1; k <= 4; ++k) m = RunningMean(m, xs[k - 1], k);
  CHECK_NEAR(m, 0.45, 1e-6);
}

TEST(RunningMeanFirstSampleIgnoresPrev) {
  // DEFAULT-pool render targets start undefined (possibly NaN/Inf as fp32): sample 1 must not read prev.
  CHECK(std::fabs(RunningMean(std::nanf(""), 0.3f, 1) - 0.3f) < 1e-7f);  // CHECK_NEAR passes NaN
  CHECK_NEAR(RunningMean(1e38f, 0.3f, 1), 0.3, 1e-7);
}

TEST(SrgbRoundTrip) {
  for (float v : {0.0f, 0.04f, 0.5f, 1.0f}) CHECK_NEAR(LinearToSrgb(SrgbToLinear(v)), v, 1e-5);
  CHECK_NEAR(SrgbToLinear(0.5f), 0.21404, 1e-4);
}

TEST(MenuTablesConsistent) {
  CHECK(lens::Count(lens::kQualitySamples) == lens::Count(lens::kQualityNames));
  for (int i = 1; i < lens::Count(lens::kFocals); ++i) CHECK(lens::kFocals[i] > lens::kFocals[i - 1]);
  char b[16];
  lens::FormatFStop(b, sizeof b, 0);
  CHECK(std::string(b) == "Off");
  lens::FormatFStop(b, sizeof b, 2.8f);
  CHECK(std::string(b) == "F/2.8");
  lens::FormatFStop(b, sizeof b, 16);
  CHECK(std::string(b) == "F/16");
  for (int f = 0; f < static_cast<int>(Follow::Count); ++f) CHECK(FollowName(static_cast<Follow>(f)) != nullptr);
}
