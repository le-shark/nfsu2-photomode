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

// Top and bottom rows in two halves at rows 2..5 from x = 2; left and right columns in two halves at columns 8..11
// from y = 8: all at least 2 pixels inside the screen, none overlapping. False for screens too small to fit them.
inline bool Layout(int w, int h, Piece out[kPieces]) {
  const int hw = (w + 1) / 2, hh = (h + 1) / 2;
  if (2 + hw > w - 2 || 8 + hh > h - 2 || 12 > w - 2) return false;
  out[0] = {0, 0, hw, 1, 2, 2};
  out[1] = {hw, 0, w - hw, 1, 2, 3};
  out[2] = {0, h - 1, hw, 1, 2, 4};
  out[3] = {hw, h - 1, w - hw, 1, 2, 5};
  out[4] = {0, 0, 1, hh, 8, 8};
  out[5] = {0, hh, 1, h - hh, 9, 8};
  out[6] = {w - 1, 0, 1, hh, 10, 8};
  out[7] = {w - 1, hh, 1, h - hh, 11, 8};
  return true;
}

}  // namespace photomode::edges
