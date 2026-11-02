#pragma once
// The screen-edge pixels of a photo under in-game FSAA (user 2026-10-21). WidescreenFix (since upstream 2b7b068,
// 2026-09-06, "nfs: msaa workaround"; refs/widescreenfix_nfsu2/MSAA.ixx 60-108) clears 1-pixel black strips on the
// screen edges every frame when MSAA is on ([GRAPHICS] HideAABug: 1 top and left, 2 all four), just before the game's
// EndScene call, so the resolve frame read back at EndScene had them: a black frame in every photo, a black cross where
// the 2x quarters meet (docs/kb/lessons.md "Photo capture").
// The fix: one more resolve frame per screen piece shows the same finished image (the mean, drawn per pixel by
// show.hlsl), but only the four edge strips, each split in two and moved inside the screen, 1:1, where nothing clears
// them; WidescreenFix's gamma is per pixel (installed postfx resource = upstream includes/postfx/postfx.fx,
// ConsoleGammaPS, drawn texel-centred by DrawScreenQuad), so those pixels come back exactly as they would have been.
// The capture then writes them over the cleared edges.

namespace photomode::edges {

struct Piece {
  int sx, sy, w, h;  // source: a rectangle of the normal resolve frame (screen pixels)
  int dx, dy;        // where the edge frame shows it (top-left)
};
constexpr int kPieces = 8;

// 2026-11-01, a tester's photo (3840 x 2160, FSAA): the strips' pixels came out mixed about 10 % with something else
// (measured; what it is, is not established). So each strip is drawn kStripPad pixels thicker on both sides, its own
// pixels repeated, and read at the centre: thin quads and anything filtering across them no longer reach the read pixels.
inline constexpr int kStripPad = 1, kStripStep = 2 * kStripPad + 1;

// Top and bottom rows in two halves at rows 3, 6, 9, 12 from x = 2; left and right columns in two halves at columns 16,
// 19, 22, 25 from y = 16: all at least 2 pixels inside the screen, none overlapping with their padding. False for
// screens too small to fit them.
inline bool Layout(int w, int h, Piece out[kPieces]) {
  const int hw = (w + 1) / 2, hh = (h + 1) / 2;
  if (2 + hw > w - 2 || 16 + hh > h - 2 || 27 > w - 2) return false;
  out[0] = {0, 0, hw, 1, 2, 3};
  out[1] = {hw, 0, w - hw, 1, 2, 6};
  out[2] = {0, h - 1, hw, 1, 2, 9};
  out[3] = {hw, h - 1, w - hw, 1, 2, 12};
  out[4] = {0, 0, 1, hh, 16, 16};
  out[5] = {0, hh, 1, h - hh, 19, 16};
  out[6] = {w - 1, 0, 1, hh, 22, 16};
  out[7] = {w - 1, hh, 1, h - hh, 25, 16};
  return true;
}

}  // namespace photomode::edges
