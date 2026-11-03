#pragma once
// Racer tags (beta feedback 2026-10-31, "a tag hovering each racer"; key T): the name the Focus Target row shows, over
// each rival. User 2026-11-01: native. FE text of the resident, otherwise unused debug package ScreenPrintf.fng
// (feng-ui.md; [game] works in a race), in the photo-mode menu's own font; photomode.cpp keeps it out of photos.
namespace photomode::tags {
void Begin();                 // this frame's labels; adds the package on first use (the menu must be open)
// Bottom-centre of the text at canvas (x, y), relative to the screen centre.
void Show(const char* text, float x, float y, bool selected);
void End();                   // hides the labels this frame did not use
void Close();                 // before the menu closes
bool IsPackage(const char* name);
}  // namespace photomode::tags
