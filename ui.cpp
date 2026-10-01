#include "ui.h"

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
}  // namespace

bool Ui::Attach() {
  feng::FEPackage* p = FindActive(kPkg);
  if (!p) {
    pkg_ = nullptr;
    labels_.clear();
    if (!requested_) {
      U2LOG("ui: showing %s", kPkg);
      feng::ShowPackage(kPkg);
      requested_ = true;
      ownsPackage_ = true;
    }
    return false;
  }
  requested_ = false;
  if (p == pkg_ && !labels_.empty()) return true;

  // Map every string object of the package (list head at pkg+0x4C, next at obj+0x04).
  pkg_ = p;
  labels_.clear();
  for (auto o = static_cast<uint8_t*>(feng::PackageObjects(p)); o; o = *reinterpret_cast<uint8_t**>(o + 4)) {
    auto obj = reinterpret_cast<feng::FEObject*>(o);
    uint32_t type = *reinterpret_cast<uint32_t*>(o + 0x18);
    if (type != 2) continue;
    Label l{obj, *reinterpret_cast<uint32_t*>(o + 0x10), "", 0, 0, 0, false};
    float cx = 0, cy = 0;
    feng::GetCenter(obj, &cx, &cy);
    U2LOG("ui: label %08X at (%.1f, %.1f) flags %08X", l.hash, cx, cy, feng::ObjectFlags(obj));
    feng::SetInvisible(obj);
    labels_.push_back(l);
  }
  U2LOG("ui: %s attached, %zu labels", kPkg, labels_.size());
  return !labels_.empty();
}

void Ui::Set(size_t i, const std::string& text, float x, float y, uint32_t color) {
  if (i >= labels_.size()) return;
  Label& l = labels_[i];
  if (l.text != text) {
    feng::Print(kPkg, l.hash, text.c_str());
    l.text = text;
  }
  if (l.x != x || l.y != y) {
    feng::SetCenter(l.obj, x, y);
    l.x = x;
    l.y = y;
  }
  if (l.color != color) {
    feng::SetColor(l.obj, color);
    l.color = color;
  }
  if (!l.visible || (feng::ObjectFlags(l.obj) & 1)) {
    feng::SetVisible(l.obj);
    l.visible = true;
  }
}

void Ui::Draw(const MenuView& v, const UiLayout& L) {
  if (!Attach()) return;
  size_t i = 0;
  float y = L.y;
  Set(i++, v.title, L.x, y, L.colorTitle);
  y += L.lineHeight * 1.5f;
  for (auto& r : v.rows) {
    uint32_t c = r.selected ? L.colorSelected : L.colorNormal;
    Set(i++, r.name, L.x, y, c);
    Set(i++, r.selected && !r.value.empty() ? "< " + r.value + " >" : r.value, L.x + L.valueOffset, y, c);
    y += L.lineHeight;
  }
  if (!v.toast.empty()) Set(i++, v.toast, L.x, y + L.lineHeight * 0.5f, L.colorTitle);
  if (!v.help.empty()) Set(i++, v.help, L.x, L.helpY, L.colorHelp);
  // Hide whatever is left from previous frames.
  for (; i < labels_.size(); ++i)
    if (labels_[i].visible) {
      feng::SetInvisible(labels_[i].obj);
      labels_[i].visible = false;
    }
}

void Ui::HideAll() {
  if (!pkg_ || FindActive(kPkg) != pkg_) {
    labels_.clear();
    pkg_ = nullptr;
    return;
  }
  for (auto& l : labels_)
    if (l.visible) {
      feng::SetInvisible(l.obj);
      l.visible = false;
    }
}
void Ui::Detach() {
  HideAll();
  if (ownsPackage_ && FindActive(kPkg)) {
    U2LOG("ui: removing %s", kPkg);
    feng::RemovePackage(kPkg);
  }
  ownsPackage_ = false;
  requested_ = false;
  pkg_ = nullptr;
  labels_.clear();
}
}  // namespace photomode
