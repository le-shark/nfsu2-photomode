#include "tags.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "u2sdk/feng.h"
#include "u2sdk/log.h"

namespace photomode::tags {
namespace {
constexpr const char* kPkg = "ScreenPrintf.fng";
constexpr int kLabels = 22;
// SCREENPRINTF48.. (FEngLib decode): 64 FEStrings in font_arial, hidden, unused by the game.
constexpr int kFirstLabel = 48;
constexpr float kTextScale = 0.75f;  // the menu's rows are large for a marker over a car
using AddFn = void(__cdecl*)(const char*, void*);
using RemoveFn = void(__cdecl*)(const char*);
using LastButtonFn = void(__cdecl*)(const char*, int);
using SetVecFn = void(__fastcall*)(void*, void*, const float*, bool);
const auto AddFng = reinterpret_cast<AddFn>(0x555E80);           // AddFngToUIObject_1; joy handlers are already the menu's
const auto RemoveFng = reinterpret_cast<RemoveFn>(0x5379A0);     // plain remove: the handlers stay for the menu (Close)
const auto SetLastButton = reinterpret_cast<LastButtonFn>(0x50B8C0);
const auto SetPosition = reinterpret_cast<SetVecFn>(0x517C10);   // thiscall, as optmenu
const auto SetSize = reinterpret_cast<SetVecFn>(0x517DB0);
constexpr uint32_t kFlagHidden = 1, kFlagVisible = 2, kFlagDirty = 0x400000;

struct Label {
  uint8_t* obj = nullptr;
  std::string text;
  bool shown = false, selected = false;
};
Label g_labels[kLabels];
bool g_open = false;
int g_used = 0;

uint32_t& Flags(uint8_t* o) { return *reinterpret_cast<uint32_t*>(o + 0x1C); }

void SetVisible(Label& l, bool on) {
  if (!l.obj || l.shown == on) return;
  l.shown = on;
  uint32_t& f = Flags(l.obj);
  f = on ? (f | kFlagVisible) & ~kFlagHidden : (f | kFlagHidden) & ~kFlagVisible;
  f |= kFlagDirty;
}

// White, or the menu title's pale green for the Focus Target: FEObject data +0x2C holds B, G, R, A ints.
void SetSelected(Label& l, bool selected) {
  if (l.selected == selected) return;
  l.selected = selected;
  auto c = *reinterpret_cast<int**>(l.obj + 0x2C);
  c[0] = selected ? 130 : 255, c[1] = selected ? 235 : 255, c[2] = selected ? 190 : 255, c[3] = 255;
  Flags(l.obj) |= kFlagDirty;
}

bool Open() {
  if (g_open) return true;
  auto menuRow = reinterpret_cast<uint8_t*>(u2::feng::FindObject("UI_PauseOptions.fng", u2::feng::HashUpper("OPTION_NAME_1")));
  if (!menuRow) return false;
  AddFng(kPkg, nullptr);
  auto pkg = u2::feng::FindPackage(kPkg);
  if (!pkg) {
    U2LOG("racer tags: %s not available", kPkg);
    return false;
  }
  *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pkg) + 0x24) = 0;  // no input: it is only text
  const uint32_t font = *reinterpret_cast<uint32_t*>(menuRow + 0x24);  // FEString font hash (0x51D730; GetFontData)
  const float size[3] = {kTextScale, kTextScale, 1};
  for (int i = 0; i < kLabels; ++i) {
    char name[24];
    std::snprintf(name, sizeof name, "SCREENPRINTF%d", kFirstLabel + i);
    Label& l = g_labels[i];
    l = {};
    l.obj = reinterpret_cast<uint8_t*>(u2::feng::FindObject(kPkg, u2::feng::HashUpper(name)));
    if (!l.obj) continue;
    *reinterpret_cast<uint32_t*>(l.obj + 0x24) = font;
    *reinterpret_cast<uint32_t*>(l.obj + 0x6C) = 1 | 8;  // centred, bottom (FEString layout 0x51C110)
    Flags(l.obj) |= kFlagDirty;
    SetSize(l.obj, nullptr, size, false);
  }
  g_open = true;
  return true;
}
}  // namespace

void Begin() {
  g_used = 0;
  Open();
}

void Show(const char* text, float x, float y, bool selected) {
  if (!g_open || g_used >= kLabels) return;
  Label& l = g_labels[g_used++];
  if (!l.obj) return;
  if (l.text != text) {
    l.text = text;
    u2::feng::Print(kPkg, *reinterpret_cast<uint32_t*>(l.obj + 0x10), text);
  }
  const float pos[3] = {x, y, 100};
  Flags(l.obj) |= kFlagDirty;
  SetPosition(l.obj, nullptr, pos, false);
  SetSelected(l, selected);
  SetVisible(l, true);
}

void End() {
  if (!g_open) return;
  for (int i = g_used; i < kLabels; ++i) SetVisible(g_labels[i], false);
}

void Close() {
  if (!g_open) return;
  g_open = false;
  SetLastButton(kPkg, 0);
  RemoveFng(kPkg);
}

bool IsPackage(const char* name) { return g_open && name && _stricmp(name, kPkg) == 0; }
}  // namespace photomode::tags
