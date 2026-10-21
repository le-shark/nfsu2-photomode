#include <cstdlib>
#include <string>

#include "follow.h"
#include "frame.h"
#include "grid.h"
#include "lens.h"
#include "rig.h"
#include "sampling.h"
#include "watermark.h"
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

TEST(FocalSteps) {
  CHECK_NEAR(lens::StepFocal(50, 1, false), 60, 1e-4);   // next lens
  CHECK_NEAR(lens::StepFocal(50, -1, false), 40, 1e-4);
  CHECK_NEAR(lens::StepFocal(73, 1, false), 85, 1e-4);   // between lenses: the next one that way
  CHECK_NEAR(lens::StepFocal(73, -1, false), 70, 1e-4);
  CHECK_NEAR(lens::StepFocal(50, 1, true), 51, 1e-4);    // fine: 2 %, at least 1 mm
  CHECK_NEAR(lens::StepFocal(200, 1, true), 204, 1e-4);
  CHECK_NEAR(lens::StepFocal(200, -1, true), 196, 1e-4);
  CHECK_NEAR(lens::StepFocal(10, 1, true), 11, 1e-4);
  CHECK_NEAR(lens::StepFocal(300, 1, false), 300, 1e-4);  // ends
  CHECK_NEAR(lens::StepFocal(10, -1, true), 10, 1e-4);
  CHECK_NEAR(lens::StepFocal(299, 1, true), 300, 1e-4);
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

TEST(SampleAdjustAppliesToMainAndRoadReflectionViews) {
  // View 1 = main view; view 4 = road reflection, looked up by the road's own screen position, so it
  // must get the same tile zoom / jitter / lens shift. World-anchored views must not.
  CHECK(IsSampledView(1));
  CHECK(IsSampledView(4));
  for (int id : {0, 2, 3, 5, 9, 16, 21}) CHECK(!IsSampledView(id));
}

TEST(QuarterUvCoversImage) {
  const float want[4][4] = {{0, 0, .5f, .5f}, {.5f, 0, 1, .5f}, {0, .5f, .5f, 1}, {.5f, .5f, 1, 1}};
  for (int q = 0; q < 4; ++q) {
    UvRect r = QuarterUv(q);
    CHECK(r.u0 == want[q][0] && r.v0 == want[q][1] && r.u1 == want[q][2] && r.v1 == want[q][3]);
  }
}

TEST(SupersampleJitterIsHalf) {
  SampleParams p{128, 1, 0, 5, true};
  u2::ProjAdjust big = SampleAdjust(p, 7, 0, 3840, 2160), one = SampleAdjust(p, 7, 0, 1920, 1080);
  CHECK_NEAR(big.jitterX, one.jitterX / 2, 1e-9);
  CHECK_NEAR(big.jitterY, one.jitterY / 2, 1e-9);
  CHECK(big.tiles == 1 && big.tx == 0 && big.ty == 0);
}

TEST(MaxTilesIsTwo) { CHECK(lens::kMaxTiles == 2); }

TEST(GlowWarmupReachesSteadyState) {
  // The streak buffer is faded by BLENDFACTOR 0xFF785AAA every frame (0x5CBB57): R 120, G 90, B 170
  // /255. Starting from black, the warm-up must leave less than 0.1 % of the steady state missing.
  for (int c : {120, 90, 170}) CHECK(std::pow(c / 255.0, kGlowWarmupFrames) < 0.001);
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

TEST(WatermarkListNames) {
  std::string n;
  CHECK(watermark::ListName(L"WATERMARK.png", &n) && n == "Custom");
  CHECK(watermark::ListName(L"watermark.PNG", &n) && n == "Custom");
  CHECK(watermark::ListName(L"WaterMark_My Logo.png", &n) && n == "My Logo");
  CHECK(watermark::ListName(L"WATERMARK_v1.2.png", &n) && n == "v1.2");
  CHECK(watermark::ListName(L"WATERMARK_jpg", &n) && n == "jpg");
  CHECK(!watermark::ListName(L"WATERMARK_.png", &n));
  CHECK(!watermark::ListName(L"WATERMARKS.png", &n));
  CHECK(!watermark::ListName(L"WATER.png", &n));
  CHECK(!watermark::ListName(L"logo.png", &n));
}

TEST(GridThirdsLines) {
  using grid::Line;
  CHECK(grid::Thickness(1080) == 2);
  CHECK(grid::Thickness(2160) == 4);
  CHECK(grid::Thickness(720) == 1);
  CHECK(grid::Thickness(480) == 1);
  Line l[grid::kMaxLines];
  CHECK(grid::Thirds(0, 0, 1920, 1080, 2, l) == 8);
  CHECK(l[0].x == 639 && l[0].w == 2 && l[0].h == 1080);  // 640 - 1
  CHECK(l[1].x == 1279 && l[1].w == 2);
  CHECK(l[2].y == 359 && l[2].h == 2 && l[2].x == 0 && l[2].w == 639);
  CHECK(l[3].x == 641 && l[3].w == 638);
  CHECK(l[4].x == 1281 && l[4].x + l[4].w == 1920);
  CHECK(l[5].y == 719);
  int area = 0;  // nothing drawn twice: the pieces add up to the lines' union
  for (const Line& x : l) area += x.w * x.h;
  CHECK(area == 2 * 2 * 1080 + 2 * 2 * 1920 - 4 * 2 * 2);
  // Inside a 1:1 photo area, the lines stay inside it.
  const frame::Rect f = frame::Fit(1920, 1080, 6);
  grid::Thirds(f.x, f.y, f.w, f.h, 2, l);
  for (const Line& x : l) CHECK(x.x >= f.x && x.y >= f.y && x.x + x.w <= f.x + f.w && x.y + x.h <= f.y + f.h);
}

TEST(AspectRatioFrames) {
  frame::Rect f = frame::Fit(1920, 1080, 0);  // Native
  CHECK(f.x == 0 && f.y == 0 && f.w == 1920 && f.h == 1080);
  f = frame::Fit(1920, 1080, 1);  // 16:9 on a 16:9 screen: no bars
  CHECK(f.x == 0 && f.y == 0 && f.w == 1920 && f.h == 1080);
  f = frame::Fit(1920, 1080, 2);  // 16:10: pillars
  CHECK(f.h == 1080 && f.w == 1728 && f.x == 96 && f.y == 0);
  f = frame::Fit(1920, 1080, 3);  // 2.37:1: letterbox
  CHECK(f.w == 1920 && f.h == 810 && f.y == 135);
  f = frame::Fit(1920, 1080, 6);  // 1:1
  CHECK(f.w == 1080 && f.h == 1080 && f.x == 420);
  f = frame::Fit(1920, 1080, 7);  // 9:16
  CHECK(f.h == 1080 && f.w == 608 && f.x == 656);
  f = frame::Fit(1920, 1080, 10);  // 1:2.37
  CHECK(f.h == 1080 && f.w == 456);
  f = frame::Fit(2560, 1600, 1);  // 16:9 on 16:10
  CHECK(f.w == 2560 && f.h == 1440 && f.y == 80);
  for (int i = 0; i < frame::kCount; ++i) {  // bars + area cover the image exactly once
    const frame::Rect a = frame::Fit(1366, 768, i);
    frame::Rect b[4];
    long long sum = static_cast<long long>(a.w) * a.h;
    for (int k = 0, n = frame::Bars(1366, 768, a, b); k < n; ++k) {
      CHECK(b[k].w > 0 && b[k].h > 0);
      sum += static_cast<long long>(b[k].w) * b[k].h;
    }
    CHECK(sum == 1366LL * 768);
    CHECK(std::string(frame::Name(i)).size() > 0);
  }
  // 2x: the whole image's area is twice the screen's, give or take a pixel of rounding.
  for (int i = 0; i < frame::kCount; ++i) {
    const frame::Rect s1 = frame::Fit(1920, 1080, i), s2 = frame::Fit(3840, 2160, i);
    CHECK(std::abs(s2.w - 2 * s1.w) <= 1 && std::abs(s2.h - 2 * s1.h) <= 1);
  }
}
