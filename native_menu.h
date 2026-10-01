#pragma once
// Native in-game options screen for photo mode.
//
// Reuses the game's own in-race options package UI_PauseOptions.fng (title box, highlight bar,
// "Label  < Value >" rows, PC help bar) with our own screen object, the way NFSU2 Extra Options
// builds custom screens (LanguageSelectScreen.h: own factory in the FNGData table + own vtable) and
// NFSU2Unlimiter builds widget rows (BuyPerformanceParts.h: UIWidgetMenu + FEStatWidget, custom
// widget vtables, FEngPrintf on the row labels). Addresses/semantics: docs/kb/native-screen.md.
#include <string>

namespace photomode::menu {

struct Callbacks {
  int rowCount;                                  // <= 11 (package has OPTION_NAME_1..11)
  std::string (*label)(int row);
  std::string (*value)(int row);
  void (*adjust)(int row, int dir);              // LEFT/RIGHT on a row
  void (*activate)(int row);                     // ACCEPT / click on a row (called from frame code)
  void (*back)();                                // BACK pressed (called from frame code)
};

// Side-panel layout: our package is drawn in its own pass with this canvas-space scale/offset
// applied on top of the FE transform (640x480 FE canvas, origin at the centre).
struct Layout {
  float scale = 0.6f;
  float x = 226, y = -82;       // canvas offset after scaling
  int backingAlpha = 140;       // 0..255 alpha of the menu backing panels (transparency)
};

bool Install(const Callbacks& cb, const Layout& layout);
const Layout& GetLayout();
bool IsMenuPackage(const char* name);  // our package (drawn in the scaled pass)
bool Open();                                     // call from frame code (not inside FE callbacks)
void Close();                                    // same
bool IsOpen();
void Tick();                                     // every frame, even when photo mode is off
void Update(const std::string& title, const std::string& status);  // per frame: redraw, run deferred actions
int CurrentRow();                                // -1 if none

}  // namespace photomode::menu
