#pragma once
// Per-sample lens / AA / tile adjustment shared by the live preview and the capture, the key
// that says when an accumulated image is still valid, and the CPU reference for the
// accumulator shaders (shaders/accum.hlsl, shaders/show.hlsl). Pure math, unit-tested.
#include <cmath>
#include <cstdint>

#include "u2sdk/u2math.h"

namespace photomode {

using u2::Vec3;

struct SampleParams {
  int samples = 128;
  int tiles = 1;            // image is tiles x tiles screens
  float lensRadius = 0;     // metres, 0 = pinhole
  float focusDist = 10;     // metres
  bool antialias = true;
};

// Everything that changes the accumulated image. Floats compare exactly: any change restarts it.
struct SampleKey {
  Vec3 eye, fwd, up;
  uint16_t fov = 0;
  SampleParams p;
  float shutterSec = 0;
};
inline bool operator==(const SampleKey& a, const SampleKey& b) {
  auto eq = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
  return eq(a.eye, b.eye) && eq(a.fwd, b.fwd) && eq(a.up, b.up) && a.fov == b.fov && a.p.samples == b.p.samples &&
         a.p.tiles == b.p.tiles && a.p.lensRadius == b.p.lensRadius && a.p.focusDist == b.p.focusDist &&
         a.p.antialias == b.p.antialias && a.shutterSec == b.shutterSec;
}

// Projection adjustment for sample s of tile `tile`: lens position on a golden-angle disc,
// Halton sub-pixel jitter (of the final image), tile zoom.
inline u2::ProjAdjust SampleAdjust(const SampleParams& p, int s, int tile, int width, int height) {
  u2::ProjAdjust a;
  a.focusDist = p.focusDist > 0.05f ? p.focusDist : 0.05f;
  if (p.lensRadius > 0) {
    float x, y;
    u2::GoldenDisc(static_cast<uint32_t>(s), static_cast<uint32_t>(p.samples), &x, &y);
    a.lensX = x * p.lensRadius;
    a.lensY = y * p.lensRadius;
  }
  if (p.antialias && p.samples > 1) {
    a.jitterX = (u2::Halton(static_cast<uint32_t>(s) + 1, 2) - 0.5f) * 2.0f / (width * p.tiles);
    a.jitterY = (u2::Halton(static_cast<uint32_t>(s) + 1, 3) - 0.5f) * 2.0f / (height * p.tiles);
  }
  a.tiles = p.tiles;
  a.tx = tile % p.tiles;
  a.ty = tile / p.tiles;
  return a;
}

// Views that get the per-sample adjustment in the 0x5C7EC0 post-hook. 1 = main view. 4 = road
// reflection (RT[1] 0x86E784, 320x240, camera 0x8760E0 shared with view 1, rendered in Render3D at
// 0x5CAB21): the road looks it up by its own clip position under view 1 (reflect vertex shaders
// in the exe, e.g. 0x432870: oT1 = 0.5 * (clip.xy / w) + 0.5), so it must be zoomed / jittered /
// lens-shifted exactly like view 1, or every tile shows the whole reflection. Views that are
// world-anchored (9 headlight projector, 0x10..0x15 env-map faces) or the mirror (3) are not.
inline bool IsSampledView(int id) { return id == 1 || id == 4; }

// Frames rendered into a fresh 2x streak buffer before a 2x capture samples: the game fades it by
// BLENDFACTOR 0xFF785AAA per frame (0x5CBB57) and adds the lights again, so glows build up to their
// steady brightness. Blue, the slowest channel (170/255 per frame), needs 18 frames to reach 99.9 %;
// 20 leaves a margin.
inline constexpr int kGlowWarmupFrames = 20;

// A rectangle in texture coordinates (0..1).
struct UvRect {
  float u0, v0, u1, v1;
};
// Quarter q (0..3: left-right, then top-bottom) of a 2x image: a 2x supersampled photo is read back
// one quarter per resolve frame, into the same slots as 2x2 tiles (tile index q).
inline UvRect QuarterUv(int q) {
  float u = (q % 2) * 0.5f, v = (q / 2) * 0.5f;
  return {u, v, u + 0.5f, v + 0.5f};
}

// sRGB transfer functions (IEC 61966-2-1); the shaders use the same constants.
inline float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float LinearToSrgb(float l) { return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1 / 2.4f) - 0.055f; }
// Mean of k samples from the mean of the first k - 1 and sample k (k is 1-based). Sample 1 never
// reads prev: a fresh render target holds undefined data (NaN/Inf as fp32) that would stick.
inline float RunningMean(float prev, float x, int k) { return k <= 1 ? x : prev + (x - prev) / static_cast<float>(k); }

}  // namespace photomode
