#pragma once
// Photo-mode menu drawn with the game's own FEng: the hidden debug labels of the always-resident
// ScreenPrintf.fng package (native font, colours and WidescreenFix scaling).
#include <cstdint>
#include <string>
#include <vector>

namespace u2::feng { struct FEObject; struct FEPackage; }

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

struct UiLayout {
  float x = -300, y = -170;   // 640x480 canvas, origin at the centre
  float lineHeight = 17;
  float valueOffset = 170;
  float helpY = 205;
  uint32_t colorNormal = 0xFFB4C8DC, colorSelected = 0xFFFFC800, colorTitle = 0xFFFFFFFF, colorHelp = 0xFF8CA0B4;
};

class Ui {
 public:
  // Makes sure ScreenPrintf.fng is active and its labels are mapped. Cheap once attached.
  bool Attach();
  void Draw(const MenuView& v, const UiLayout& l);
  void HideAll();
  void Detach();   // removes the package again if we were the ones who showed it
  u2::feng::FEPackage* Package() const { return pkg_; }

 private:
  struct Label {
    u2::feng::FEObject* obj;
    uint32_t hash;
    std::string text;
    uint32_t color;
    float x, y;
    bool visible;
  };
  void Set(size_t i, const std::string& text, float x, float y, uint32_t color);
  u2::feng::FEPackage* pkg_ = nullptr;
  std::vector<Label> labels_;
  bool requested_ = false;
  bool ownsPackage_ = false;
  int dumpPending_ = 0;
  void DumpFirstLabel();
};

}  // namespace photomode
