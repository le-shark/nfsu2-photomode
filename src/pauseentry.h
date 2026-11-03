#pragma once
// "Photo Mode" icon in the game's pause menu (pauseentry.cpp).

namespace photomode::pause {
bool Install();      // hooks only; call before hook::EnableAll. False = no icon (logged), photo mode still works
bool TakeRequest();  // true once after the icon was selected (the game is resuming the race)
// Back to the pause menu photo mode was opened from: the game's own pause-key handler for the port that paused.
// False while the game would refuse (pause key blocked, no player on that port); call again next frame.
bool ReopenPauseMenu();
}  // namespace photomode::pause
