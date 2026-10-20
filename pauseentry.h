#pragma once
// "Photo Mode" icon in the game's pause menu (pauseentry.cpp).

namespace photomode::pause {
bool Install();      // hooks only; call before hook::EnableAll. False = no icon (logged), photo mode still works
bool TakeRequest();  // true once after the icon was selected (the game is resuming the race)
}  // namespace photomode::pause
