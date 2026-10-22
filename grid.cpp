#include "grid.h"

#include <windows.h>
#include <d3d9.h>

#include "overlay.h"
#include "u2sdk/log.h"

namespace photomode::grid {

void Overlay::Draw(IDirect3DDevice9* dev, const Line* rects, int count, uint32_t argb) {
  if (!dev || failed_ || count <= 0) return;
  if (!overlay::MakeBlock(dev, overlay::Kind::Coloured, &state_)) {
    U2LOG("grid: state block failed; not shown");
    failed_ = true;
    return;
  }
  overlay::Scope scope(dev, state_, overlay::Kind::Coloured);
  if (!scope.Ok()) return;
  struct V { float x, y, z, rhw; DWORD c; };
  V q[kMaxLines * 6];
  int n = 0;
  const DWORD kColour = argb;
  if (count > kMaxLines) count = kMaxLines;
  for (int i = 0; i < count; ++i) {  // two triangles each; -0.5 puts the quad edges on pixel edges
    const Line& l = rects[i];
    const float a = l.x - 0.5f, t = l.y - 0.5f, r = l.x + l.w - 0.5f, b = l.y + l.h - 0.5f;
    const V c[6] = {{a, t, 0, 1, kColour}, {r, t, 0, 1, kColour}, {a, b, 0, 1, kColour},
                    {r, t, 0, 1, kColour}, {r, b, 0, 1, kColour}, {a, b, 0, 1, kColour}};
    for (const V& v : c) q[n++] = v;
  }
  dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, q, sizeof(V));
}

void Overlay::Free() {
  if (state_) {
    state_->Release();
    state_ = nullptr;
  }
  failed_ = false;
}

}  // namespace photomode::grid
