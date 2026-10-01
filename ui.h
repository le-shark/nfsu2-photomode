#pragma once
// Photo-mode menu drawn with the game's own FEng, following yugecin's working nfsu2-re code
// (nfsu2-re-hooks/debug-custom-markers-debug.c): push the always-resident debug package
// ScreenPrintf.fng with AddFngToUIObject_1(pkg, 0) once, then only FEPrintf text into its labels.
// The package is never removed again (nfsu2-re never does; removing it broke gameplay input, and
// pushing it any other way crashed). Labels are "hidden" by printing an empty string.
// Its 64 labels sit in a native 4 x 16 grid (x = -231.5, -78.5, 75.5, 226.5;
// y = -219.5 .. 80.5 in steps of 20 on the 640x480 FE canvas).
#include <cstdint>
#include <string>
#include <vector>

namespace u2::feng { struct FEPackage; }

namespace photomode {

struct MenuRow {
  std::string name, value;
  bool selected = false;
};

struct MenuView {
  std::string title;
  std::vector<MenuRow> rows;
  std::string help;
  std::string toast;
};

class Ui {
 public:
  bool Attach();                       // pushes / maps ScreenPrintf.fng; true when labels are ready
  void Draw(const MenuView& v);
  void Clear();                        // prints "" into every label
  u2::feng::FEPackage* Package() const { return pkg_; }

  static constexpr int kCols = 4, kRows = 16;

 private:
  struct Label {
    uint8_t* obj = nullptr;
    uint32_t hash = 0;
    std::string text;
  };
  void Set(int col, int row, const std::string& text);
  u2::feng::FEPackage* pkg_ = nullptr;
  Label grid_[kCols][kRows];
  bool mapped_ = false;
  bool requested_ = false;
  int logFlags_ = 0;
};

}  // namespace photomode
