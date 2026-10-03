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

// sRGB transfer functions (IEC 61966-2-1); the shaders use the same constants.
inline float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float LinearToSrgb(float l) { return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1 / 2.4f) - 0.055f; }
// Mean of k samples from the mean of the first k - 1 and sample k (k is 1-based). Sample 1 never
// reads prev: a fresh render target holds undefined data (NaN/Inf as fp32) that would stick.
inline float RunningMean(float prev, float x, int k) { return k <= 1 ? x : prev + (x - prev) / static_cast<float>(k); }

}  // namespace photomode
