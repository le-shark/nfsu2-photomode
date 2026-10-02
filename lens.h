#pragma once
// Photographic parameters and the tables behind the menu. Pure code (unit-tested).
#include <cmath>
#include <cstdio>

namespace photomode::lens {

inline constexpr float kFocals[] = {10, 12, 14, 16, 18, 20, 24, 28, 35, 40, 50, 60, 70, 85, 100, 135, 150, 200, 250, 300};
inline constexpr float kFStops[] = {0 /*off*/, 1.0f, 1.2f, 1.4f, 2, 2.8f, 4, 5.6f, 8, 11, 16, 22};
inline constexpr int kDefaultFStop = 5;  // F/2.8
inline constexpr float kShutters[] = {0 /*off*/, 1 / 1000.f, 1 / 500.f, 1 / 250.f, 1 / 125.f, 1 / 60.f,
                                      1 / 30.f,  1 / 15.f,   1 / 8.f,   1 / 4.f,   1 / 2.f,   1};
inline constexpr const char* kShutterNames[] = {"OFF",  "1/1000", "1/500", "1/250", "1/125", "1/60",
                                                "1/30", "1/15",   "1/8",   "1/4",   "1/2",   "1 s"};
inline constexpr int kQualitySamples[] = {32, 128, 256};
inline constexpr const char* kQualityNames[] = {"DRAFT", "HIGH", "ULTRA"};
inline constexpr int kMaxTiles = 4;

template <typename T, int N>
constexpr int Count(const T (&)[N]) { return N; }

// Thin-lens aperture radius in metres for a 35 mm-equivalent focal length (mm) and f-number.
inline float ApertureRadiusM(float focalMm, float fstop) {
  return fstop <= 0 ? 0 : (focalMm / fstop) * 0.5f / 1000.0f;
}

// Total depth of field (m) for focus distance s (m), using circle of confusion c = 0.03 mm.
// Returns a large number when everything from near to infinity is sharp.
inline float DepthOfFieldM(float focalMm, float fstop, float s) {
  if (fstop <= 0) return 1e4f;
  float f = focalMm / 1000.0f, c = 0.03f / 1000.0f;
  float H = f * f / (fstop * c) + f;  // hyperfocal distance
  if (s >= H) return 1e4f;
  float nearD = s * (H - f) / (H + s - 2 * f);
  float farD = s * (H - f) / (H - s);
  return farD - nearD;
}

// Index of the table entry closest to v.
template <int N>
inline int Closest(const float (&t)[N], float v) {
  int best = 0;
  for (int i = 1; i < N; ++i)
    if (std::fabs(t[i] - v) < std::fabs(t[best] - v)) best = i;
  return best;
}

inline void FormatFStop(char* buf, size_t n, float f) {
  if (f <= 0) std::snprintf(buf, n, "OFF");
  else if (f < 10) std::snprintf(buf, n, "F/%.1f", f);
  else std::snprintf(buf, n, "F/%.0f", f);
}

}  // namespace photomode::lens
