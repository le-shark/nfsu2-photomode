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
  bool (*isAction)(int row);                     // button row: no < > arrows, LEFT/RIGHT ignored
};

// Sidebar layout, in FE canvas units (480 high, origin at the centre; with WidescreenFix the
// visible width is 480 * aspect). The package is drawn at native size; only the window frame is
// reshaped into a full-height bar at the left edge and the row columns are moved to fit it.
struct Layout {
  float width = 340;            // bar width (outer black backing)
  float margin = 16;            // gap between the bar and the screen edges
  float valueWidth = 110;       // width of the "< value >" column between the arrows
  int backingAlpha = 140;       // 0..255 alpha of the menu backing panels (transparency)
};

bool Install(const Callbacks& cb, const Layout& layout);
const Layout& GetLayout();
bool IsMenuPackage(const char* name);  // our package while the menu is open
bool Open();                                     // call from frame code (not inside FE callbacks)
void Close();                                    // same
bool IsOpen();
// Hide/show without unloading: the package stays loaded (no rebuild hitch); while hidden it is
// not drawn (see IsHidden) and takes no input.
void SetHidden(bool hidden);
bool IsHidden();
void Tick();                                     // every frame, even when photo mode is off
void Update(const std::string& title, const std::string& status);  // per frame: redraw, run deferred actions
int CurrentRow();                                // -1 if none

}  // namespace photomode::menu
