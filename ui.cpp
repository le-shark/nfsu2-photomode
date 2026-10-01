#include "ui.h"

#include <algorithm>
#include <cstring>

#include "u2sdk/feng.h"
#include "u2sdk/log.h"

namespace photomode {
namespace feng = u2::feng;
namespace {
constexpr const char* kPkg = "ScreenPrintf.fng";

feng::FEPackage* FindActive(const char* name) {
  for (auto p = feng::FirstActivePackage(); p; p = feng::NextPackage(p)) {
    const char* n = feng::PackageName(p);
    if (n && _stricmp(n, name) == 0) return p;
  }
  return nullptr;
}

struct Found {
  uint32_t hash;
  float x, y;
};
}  // namespace

bool Ui::Attach() {
  feng::FEPackage* p = FindActive(kPkg);
  if (!p) {
    mapped_ = false;
    pkg_ = nullptr;
    if (!requested_) {
      U2LOG("ui: AddFngToUIObject_1(%s)", kPkg);
      feng::ShowPackage(kPkg);
      requested_ = true;
    }
    return false;
  }
  requested_ = false;
  if (p == pkg_ && mapped_) return true;
  pkg_ = p;

  // Collect the string objects (list at pkg+0x4C, next at obj+4) and sort them into the grid by
  // their authored positions: columns left->right, rows top->bottom.
  std::vector<Found> found;
  for (auto o = static_cast<uint8_t*>(feng::PackageObjects(p)); o; o = *reinterpret_cast<uint8_t**>(o + 4)) {
    if (*reinterpret_cast<uint32_t*>(o + 0x18) != 2) continue;
    Found f{*reinterpret_cast<uint32_t*>(o + 0x10), 0, 0};
    feng::GetCenter(reinterpret_cast<feng::FEObject*>(o), &f.x, &f.y);
    found.push_back(f);
  }
  if (found.size() < static_cast<size_t>(kCols * kRows)) {
    U2LOG("ui: only %zu labels in %s", found.size(), kPkg);
    return false;
  }
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  for (int c = 0; c < kCols; ++c)
    for (int r = 0; r < kRows; ++r) grid_[c][r] = Label{found[c * kRows + r].hash, std::string("\x01")};  // force first print
  mapped_ = true;
  U2LOG("ui: mapped %d x %d labels, top-left %08X at (%.1f, %.1f)", kCols, kRows, grid_[0][0].hash, found[0].x, found[0].y);
  return true;
}

void Ui::Set(int col, int row, const std::string& text) {
  if (col < 0 || col >= kCols || row < 0 || row >= kRows) return;
  Label& l = grid_[col][row];
  if (l.text == text) return;
  feng::Print(kPkg, l.hash, text.c_str());
  l.text = text;
}

void Ui::Draw(const MenuView& v) {
  if (!Attach()) return;
  std::string cells[kCols][kRows];
  cells[0][0] = v.title;
  int r = 2;
  for (auto& row : v.rows) {
    if (r >= kRows) break;
    cells[0][r] = (row.selected ? "> " : "   ") + row.name;
    cells[1][r] = row.selected && !row.value.empty() ? "< " + row.value + " >" : row.value;
    ++r;
  }
  if (!v.toast.empty() && r + 1 < kRows) cells[0][r + 1] = v.toast;
  if (!v.help.empty()) cells[0][kRows - 1] = v.help;
  for (int c = 0; c < kCols; ++c)
    for (int i = 0; i < kRows; ++i) Set(c, i, cells[c][i]);
}

void Ui::Clear() {
  if (!mapped_ || FindActive(kPkg) != pkg_) {
    mapped_ = false;
    pkg_ = nullptr;
    return;
  }
  for (int c = 0; c < kCols; ++c)
    for (int i = 0; i < kRows; ++i) Set(c, i, "");
}
}  // namespace photomode
