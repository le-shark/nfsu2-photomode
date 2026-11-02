#pragma once
// User 2026-11-01: the world's sound off while photo mode is open; music and the menu's sounds stay. In memory only, so a
// crash or Alt+F4 leaves nothing behind. The game's own sound pause: EAXSound::Pause 0x4810F0 (thiscall on [0x82B884],
// mode, immediate) as its in-game callers 0x60B4CE / 0x60B68F use it: immediate 0 and mode 0xD (no music pause, 0x48111E,
// 0x4823AE). Immediate 1 (+0x14) forces every mix channel silent, music and FE too (0x467460, 0x467F15; [game] 2026-11-01).
// The gate 0x45E3D0 silences only once +0x18 + 1 s < +0x1C (+0x1C sums the update's dt): +0x18 is set back so it is at once.
// The FE voices' path (0x479473, 0x4794FD) does not read the pause. Undo: Unpause 0x481210(1).
// Rejected: SFX / engine / tire volumes at 0: FE sounds share the SFX channel (SOUND\MixMaps), and a save could keep them.
#include <cstdint>

#include "u2sdk/game.h"

namespace photomode::mute {
inline void Set(bool on) {
  static bool muted = false;
  auto sound = u2::Var<uint8_t*>(0x82B884);
  if (on == muted || !sound) return;
  muted = on;
  constexpr int kKeepMusic = 0xD;
  if (on) {
    reinterpret_cast<void(__fastcall*)(void*, void*, int, int)>(0x4810F0)(sound, nullptr, kKeepMusic, 0);
    *reinterpret_cast<float*>(sound + 0x18) = *reinterpret_cast<float*>(sound + 0x1C) - 2;
  } else {
    reinterpret_cast<void(__fastcall*)(void*, void*, int)>(0x481210)(sound, nullptr, 1);
  }
}
}  // namespace photomode::mute
