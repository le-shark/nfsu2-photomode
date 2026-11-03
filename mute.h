#pragma once
// World Sound off while photo mode is open: the sound paused as the game's pause menu does, so only the menu (FE) sounds
// stay (user 2026-11-03: music may stop too; the music-keeping mode 0xD left nearby cars' engines audible, [game]
// 2026-11-03, while the pause menu is silent there). In memory only, so a crash or Alt+F4 leaves nothing behind.
// EAXSound::Pause 0x4810F0 (thiscall on [0x82B884], mode, immediate) with mode -1 and immediate 0, as the race pause
// 0x60B4CE (push 0, push -1); mode -1 also pauses the music stream ([EAXSound+0xA0] state 5, 0x739030(h, 1)) and the
// speech channels (0x46C8D0 on [0x82BA30]). Undone with Unpause 0x481210(-1), the game's own counterpart (the focus pair
// 0x5CCDD1 / 0x5CCDF0): it resumes the music (0x4813C1) and the speech. Immediate 1 (+0x14) forces every mix channel
// silent, FE too (0x467460, 0x467F15; [game] 2026-11-01). The gate 0x45E3D0 silences only once +0x18 + 1 s < +0x1C
// (+0x1C sums the update's dt): +0x18 is set back so it is at once. The FE voices' path (0x479473, 0x4794FD) does not
// read the pause. A sound already paused when we open (+0x13, e.g. the game's own pause) is left alone, and only our own
// pause is undone, so the game's pause stays the game's.
// Rejected: SFX / engine / tire volumes at 0: FE sounds share the SFX channel (SOUND\MixMaps), and a save could keep them.
#include <cstdint>

#include "u2sdk/game.h"

namespace photomode::mute {
inline void Set(bool on) {
  static bool muted = false;  // paused by us
  auto sound = u2::Var<uint8_t*>(0x82B884);
  if (!sound) return;
  constexpr int kLikePauseMenu = -1;
  if (on) {
    if (muted || sound[0x13]) return;
    muted = true;
    reinterpret_cast<void(__fastcall*)(void*, void*, int, int)>(0x4810F0)(sound, nullptr, kLikePauseMenu, 0);
    *reinterpret_cast<float*>(sound + 0x18) = *reinterpret_cast<float*>(sound + 0x1C) - 2;
  } else if (muted) {
    muted = false;
    reinterpret_cast<void(__fastcall*)(void*, void*, int)>(0x481210)(sound, nullptr, kLikePauseMenu);
  }
}
}  // namespace photomode::mute
