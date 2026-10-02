#include "native_menu.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "u2sdk/feng.h"
#include "u2sdk/game.h"
#include "u2sdk/log.h"
#include "u2sdk/mem.h"

namespace photomode::menu {
namespace {
using u2::Var;

// ---- game functions (see docs/kb/native-screen.md for the evidence behind each) -------------
constexpr const char* kPkg = "UI_PauseOptions.fng";
constexpr uintptr_t kFactorySlot = 0x7F81F4;      // FNGData[UI_PauseOptions.fng].create, orig 0x4EB650
constexpr uintptr_t kPauseKeyBlock = 0x861E78;    // Career+0x7138: JOY_EVENT_PAUSE handler ignores pause when set
constexpr int kMaxRows = 11;
constexpr unsigned kMsgLeft = 0x9120409E, kMsgRight = 0xB5971BF1, kMsgAccept = 0x406415E3, kMsgBack = 0x911AB364,
                   kMsgSelect = 0xC407210;

template <typename F>
F Fn(uintptr_t a) { return reinterpret_cast<F>(a); }
auto GameMalloc = Fn<void*(__cdecl*)(size_t)>(0x575620);
auto UIWidgetMenu_ctor = Fn<void*(__fastcall*)(void*, void*, void*)>(0x552780);       // thiscall(sd)
auto UIWidgetMenu_Notify = Fn<void(__fastcall*)(void*, void*, unsigned, void*, int, int)>(0x556020);
auto UIWidgetMenu_SetInitialOption = Fn<void(__fastcall*)(void*, void*, int)>(0x539650);  // Unlimiter InGameFunctions.h:182
auto UIWidgetMenu_AddToggleOption = Fn<int(__fastcall*)(void*, void*, void*, bool)>(0x54F170);
auto FEToggleWidget_ctor = Fn<void*(__fastcall*)(void*, void*, float*, float*, float*, float*, bool)>(0x51E230);
auto FEToggleWidget_Show = Fn<void(__fastcall*)(void*, void*)>(0x50D5D0);  // FEngSetVisible on title, data, arrows, backing
auto FEToggleWidget_BlinkArrows = Fn<void(__fastcall*)(void*, void*, unsigned)>(0x51E6B0);
auto FEngPrintf = Fn<void(__cdecl*)(void*, const char*, ...)>(0x537BE0);               // Unlimiter InGameFunctions.h:135
auto AddFngToUIObject_1 = Fn<void(__cdecl*)(const char*, void*)>(0x555E80);            // push + AddJoyHandlers
auto RemoveFngAndJoyHandlers = Fn<void(__cdecl*)(const char*)>(0x558F00);              // what the game pairs with it
auto RemoveFNGFromUIObject = Fn<void(__cdecl*)(const char*)>(0x5379A0);
auto IsFNGLoaded = Fn<bool(__cdecl*)(const char*)>(0x52CF60);
auto FEngSetLastButton = Fn<void(__cdecl*)(const char*, int)>(0x50B8C0);
// Selects the menu keyboard action map while racing (writes byte 0x870754, which the per-frame map
// selector 0x5CFBF0 checks). The game does the same for the in-world dyno screen (0x557AEE).
auto SetMenuKeyboardMap = Fn<void(__cdecl*)(char)>(0x5B7ED0);
using FactoryFn = void*(__cdecl*)(void*);

// ---- state ----------------------------------------------------------------------------------
Callbacks g_cb{};
Layout g_layout{};
FactoryFn g_origFactory = nullptr;
bool g_wantOurs = false;     // next UI_PauseOptions.fng construction is ours
int g_wantFrames = 0;
void* g_menu = nullptr;      // our UIWidgetMenu while open
bool g_open = false;
struct Row {
  uint8_t* widget;
  int id;
  std::string label, value;
};
std::vector<Row> g_rows;
int g_pendingActivate = -1;
int g_releasePauseBlockIn = 0;  // frames until the pause-key block is lifted after closing
int g_inputLogIn = 0;
bool g_pendingBack = false;
bool g_hidden = false;
uint32_t g_savedPortMask;  // our package's pkg+0x24 while hidden
std::string g_title, g_status;

void* g_menuVtbl[5];
void* g_toggleVtbl[17];

constexpr size_t kWidgetSize = 0x58;  // 0x54 FEToggleWidget + our row id (0x58 variants exist in-game)
int& RowId(void* w) { return *reinterpret_cast<int*>(static_cast<uint8_t*>(w) + 0x54); }
void* LabelString(void* w) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(w) + 0x2C); }
void* ValueString(void* w) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(w) + 0x30); }
bool IsAction(void* w) { return g_cb.isAction && g_cb.isAction(RowId(w)); }

Row* FindRow(void* w) {
  for (auto& r : g_rows)
    if (r.widget == w) return &r;
  return nullptr;
}

// Draw: write label and value into the row's OPTION_NAME_n / OPTION_DATA_n strings.
void DrawRow(void* w, bool force) {
  int id = RowId(w);
  std::string label = g_cb.label(id), value = g_cb.value(id);
  Row* r = FindRow(w);
  if (r && !force && r->label == label && r->value == value) return;
  if (LabelString(w)) FEngPrintf(LabelString(w), "%s", label.c_str());
  if (ValueString(w)) FEngPrintf(ValueString(w), "%s", value.c_str());
  if (r) {
    r->label = label;
    r->value = value;
  }
}

void __fastcall ToggleDraw(void* w, void*) { DrawRow(w, true); }

// Show (slot 5): button rows hide their arrow images (+0x4C/+0x50) again, as the widget's own
// Hide 0x50D610 does. The highlight scripts only animate colour, so the arrows stay hidden.
void __fastcall ToggleShow(void* w, void*) {
  FEToggleWidget_Show(w, nullptr);
  if (!IsAction(w)) return;
  for (int off : {0x4C, 0x50})
    u2::feng::SetInvisible(*reinterpret_cast<u2::feng::FEObject**>(static_cast<uint8_t*>(w) + off));
}

// Act: same shape as the game's toggle rows (e.g. 0x4C3C70): change value, BlinkArrows, Draw.
void __fastcall ToggleAct(void* w, void*, const char* /*pkg*/, unsigned msg) {
  if ((msg == kMsgLeft || msg == kMsgRight) && !IsAction(w)) {
    g_cb.adjust(RowId(w), msg == kMsgRight ? 1 : -1);
    FEToggleWidget_BlinkArrows(w, nullptr, msg);
    DrawRow(w, true);
  } else if (msg == kMsgSelect) {
    g_pendingActivate = RowId(w);
  }
}

void HideScrollbar(const char* pkg);

// Screen message handler: base UIWidgetMenu first (navigation, LEFT/RIGHT -> row Act, tick).
void __fastcall MenuNotify(void* self, void*, unsigned msg, void* obj, int p1, int p2) {
  UIWidgetMenu_Notify(self, nullptr, msg, obj, p1, p2);
  HideScrollbar(*reinterpret_cast<const char**>(static_cast<uint8_t*>(self) + 4));
  if (msg == kMsgAccept) {
    void* cur = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x54);
    if (cur) g_pendingActivate = RowId(cur);
  } else if (msg == kMsgBack) {
    g_pendingBack = true;  // never tear down from inside the FE update
  }
}

// Backing panels only have an INIT script, so alpha changes after startup stick.
// Group children: first child at +0x64, next sibling at +0x04 (see FEngSetVisible 0x50CA50).
void SetAlphaRecursive(uint8_t* obj, int alpha) {
  for (; obj; obj = *reinterpret_cast<uint8_t**>(obj + 4)) {
    auto data = *reinterpret_cast<int**>(obj + 0x2C);
    if (data) {
      data[3] = data[3] * alpha / 255;                       // FEObjData color: B, G, R, A ints
      *reinterpret_cast<uint32_t*>(obj + 0x1C) |= 0x400000;  // needs update (as FEngSetColor 0x51D843)
    }
    if (*reinterpret_cast<uint32_t*>(obj + 0x18) == 5) SetAlphaRecursive(*reinterpret_cast<uint8_t**>(obj + 0x64), alpha);
  }
}

void ApplyBacking(const char* pkg) {
  if (g_layout.backingAlpha >= 255) return;
  for (const char* name : {"BIG MENU BACKING", "SMALLER MENU BACKING"}) {
    auto group = reinterpret_cast<uint8_t*>(u2::feng::FindObject(pkg, u2::feng::HashUpper(name)));
    if (!group) {
      U2LOG("menu: %s not found", name);
      continue;
    }
    SetAlphaRecursive(*reinterpret_cast<uint8_t**>(group + 0x64), g_layout.backingAlpha);
  }
}

// ---- sidebar layout ---------------------------------------------------------------------------
// Everything stays at native size: the window frame is reshaped into a full-height bar at the left
// edge and the row columns are moved into it. Objects are changed with the engine's own setters,
// as the game does at 0x4C6793 (flags |= 0x400000; SetSize; SetPosition). Both rewrite the base key
// of the matching track in every script (0x5178D0), so INIT and the other scripts keep the new values.
auto FEObject_SetPosition = Fn<void(__fastcall*)(void*, void*, const float*, bool)>(0x517C10);  // thiscall, ret 8
auto FEObject_SetSize = Fn<void(__fastcall*)(void*, void*, const float*, bool)>(0x517DB0);      // thiscall, ret 8

// FEObject: +0x04 next sibling, +0x2C data, group +0x64 first child. Data (FEngLib BaseObject.Read):
// +0x00 colour, +0x10 pivot, +0x1C position, +0x28 rotation quaternion (x,y,z,w), +0x38 size.
// Positions are the object centre, relative to the parent group (topLeft = pos - size/2, 0x51D09E).
uint8_t* Next(uint8_t* o) { return *reinterpret_cast<uint8_t**>(o + 4); }
uint8_t* FirstChild(uint8_t* g) { return *reinterpret_cast<uint8_t**>(g + 0x64); }
float* Pos(uint8_t* o) { return reinterpret_cast<float*>(*reinterpret_cast<uint8_t**>(o + 0x2C) + 0x1C); }
float* Rot(uint8_t* o) { return reinterpret_cast<float*>(*reinterpret_cast<uint8_t**>(o + 0x2C) + 0x28); }
float* Size(uint8_t* o) { return reinterpret_cast<float*>(*reinterpret_cast<uint8_t**>(o + 0x2C) + 0x38); }
// Frame edges are 8 x 574 images turned a quarter about Z (quaternion z = +-0.707), so their
// on-screen width and height are swapped.
bool QuarterTurn(uint8_t* o) { return std::fabs(std::fabs(Rot(o)[2]) - 0.70710678f) < 0.05f; }

// w/h are magnitudes; the original sign is kept (negative sizes mirror an image).
void SetGeometry(uint8_t* o, float x, float y, float w, float h) {
  float pos[3] = {x, y, Pos(o)[2]};
  float size[3] = {std::copysign(w, Size(o)[0]), std::copysign(h, Size(o)[1]), Size(o)[2]};
  *reinterpret_cast<uint32_t*>(o + 0x1C) |= 0x400000;
  FEObject_SetSize(o, nullptr, size, false);
  FEObject_SetPosition(o, nullptr, pos, false);
}

struct Rect {
  float l, t, r, b;
};

uint8_t* Obj(const char* pkg, const char* name) {
  auto o = reinterpret_cast<uint8_t*>(u2::feng::FindObject(pkg, u2::feng::HashUpper(name)));
  if (!o) U2LOG("menu: %s not found", name);
  return o;
}

// Screen-space extent of a group's children (origin = the group's absolute position).
bool ChildBounds(uint8_t* group, float ox, float oy, Rect& out) {
  bool any = false;
  for (uint8_t* c = FirstChild(group); c; c = Next(c)) {
    float hw = std::fabs(Size(c)[0]) / 2, hh = std::fabs(Size(c)[1]) / 2;
    if (QuarterTurn(c)) std::swap(hw, hh);
    float x = ox + Pos(c)[0], y = oy + Pos(c)[1];
    Rect r{x - hw, y - hh, x + hw, y + hh};
    out = any ? Rect{(std::min)(out.l, r.l), (std::min)(out.t, r.t), (std::max)(out.r, r.r), (std::max)(out.b, r.b)} : r;
    any = true;
  }
  return any;
}

// 9-slice mapping along one axis: pieces spanning the middle stretch (keeping their distance to
// both edges); the others keep their size and follow the nearer edge.
void MapAxis(float lo, float hi, float oldLo, float oldHi, float newLo, float newHi, float& outC, float& outE) {
  if (hi - lo > (oldHi - oldLo) / 2) {
    lo = newLo + (lo - oldLo);
    hi = newHi - (oldHi - hi);
  } else {
    float d = (lo + hi) / 2 < (oldLo + oldHi) / 2 ? newLo - oldLo : newHi - oldHi;
    lo += d;
    hi += d;
  }
  outC = (lo + hi) / 2;
  outE = (hi - lo) / 2;
}

// Reshape one frame group (rounded box / outline). Its distance to the reference box is kept.
void ReshapeFrame(uint8_t* group, float ox, float oy, const Rect& ref, const Rect& bar) {
  Rect old;
  if (!group || !ChildBounds(group, ox, oy, old)) return;
  Rect dst{bar.l + (old.l - ref.l), bar.t + (old.t - ref.t), bar.r - (ref.r - old.r), bar.b - (ref.b - old.b)};
  for (uint8_t* c = FirstChild(group); c; c = Next(c)) {
    float hw = std::fabs(Size(c)[0]) / 2, hh = std::fabs(Size(c)[1]) / 2;
    bool turned = QuarterTurn(c);
    if (turned) std::swap(hw, hh);
    float x = ox + Pos(c)[0], y = oy + Pos(c)[1], nx, ny, nhw, nhh;
    MapAxis(x - hw, x + hw, old.l, old.r, dst.l, dst.r, nx, nhw);
    MapAxis(y - hh, y + hh, old.t, old.b, dst.t, dst.b, ny, nhh);
    if (turned) std::swap(nhw, nhh);
    SetGeometry(c, nx - ox, ny - oy, 2 * nhw, 2 * nhh);
  }
}

void HideObject(const char* pkg, const char* name) {
  if (auto o = u2::feng::FindObject(pkg, u2::feng::HashUpper(name))) u2::feng::SetInvisible(o);
}

// The scroll bar is shown again by the base handler every tick (0x544490 -> 0x538080), so this
// runs after every base message. All rows fit, so it is never needed.
void HideScrollbar(const char* pkg) {
  for (const char* n : {"SCROLLBAR_BACKING", "SCROLLBAR_HANDLE", "SCROLLBAR_ARROW_1", "SCROLLBAR_ARROW_2"})
    HideObject(pkg, n);
}

// Called after the UIWidgetMenu ctor (which read the row geometry from TITLE_MASTER/DATA_MASTER,
// 0x51FF80) and before the rows are added (each row copies these fields, FEStatWidget ctor 0x51E1B0).
void ApplySidebar(uint8_t* m, const char* pkg, int rows) {
  *reinterpret_cast<int*>(m + 0x134) = rows;  // visible rows (ctor default 7): everything fits, no scrolling
  // The group's real name ends with a space: hash 0x8D7E0C53 (FEngLib decode and the nfsu2-re
  // runtime object dump of UI_PauseOptionsMain.fng, docs.txt).
  uint8_t* nig = Obj(pkg, "NON_INTERACTIVE_GROUP ");
  uint8_t* big = Obj(pkg, "BIG MENU BACKING");
  Rect ref;
  if (!nig || !big || !ChildBounds(big, Pos(nig)[0] + Pos(big)[0], Pos(nig)[1] + Pos(big)[1], ref)) return;
  // Visible canvas: x = +-1/[0x79AC10] (WidescreenFix hor+ rewrites 2/640, Frontend.ixx), y = +-240.
  const float halfW = 1.0f / Var<float>(0x79AC10);
  const Rect bar{-halfW + g_layout.margin, -240 + g_layout.margin, -halfW + g_layout.margin + g_layout.width,
                 240 - g_layout.margin};
  const float dxL = bar.l - ref.l, dxR = bar.r - ref.r, dyT = bar.t - ref.t;

  // Frame: black backing, white inner panel, green outline.
  for (const char* name : {"BIG MENU BACKING", "SMALLER MENU BACKING", "BORDER"}) {
    uint8_t* g = Obj(pkg, name);
    if (g) ReshapeFrame(g, Pos(nig)[0] + Pos(g)[0], Pos(nig)[1] + Pos(g)[1], ref, bar);
  }
  // Header texts follow the top-left / top-right corner.
  if (uint8_t* h = Obj(pkg, "HEADER_TEXT")) SetGeometry(h, Pos(h)[0] + dxL, Pos(h)[1] + dyT, Size(h)[0], Size(h)[1]);
  if (uint8_t* h = Obj(pkg, "HEADER_TEXT2")) SetGeometry(h, Pos(h)[0] + dxR, Pos(h)[1] + dyT, Size(h)[0], Size(h)[1]);

  // Rows (UIWidgetMenu fields, 0x51FF80): +0xF4/+0xF8 start (top-left of row 1), +0xFC/+0x100 next
  // row, +0x104 row width, +0x10C max title size, +0x114 max data size, +0x11C/+0x120 data top-left.
  // The arrows sit just outside the data column (toggle Position 0x52DAC0).
  auto f = [m](int off) -> float& { return *reinterpret_cast<float*>(m + off); };
  constexpr float kArrow = 24, kRightPad = 16;  // arrow image width (JSON), gap to the bar edge
  // Rows end inside the inner panel (SMALLER MENU BACKING ends 32 above the outer frame) with a
  // small gap; shrink the step if they don't fit.
  const float rowsBottom = bar.b - 38;
  float startX = f(0xF4) + dxL, startY = f(0xF8) + dyT;
  if (rows > 0) f(0x108) = (std::min)(f(0x108), std::floor((rowsBottom - startY) / rows));
  float dataW = g_layout.valueWidth, dataX = bar.r - kRightPad - kArrow - dataW;
  f(0xF4) = f(0xFC) = startX;
  f(0xF8) = f(0x100) = startY;
  f(0x11C) = dataX;
  f(0x120) = startY;
  f(0x114) = dataW;
  f(0x10C) = dataX - kArrow - startX;
  f(0x104) = std::fabs(startX - (dataX + dataW));

  // Parts this screen never uses: slider pieces, console back prompt, scroll bar.
  char name[32];
  for (const char* part : {"SLIDER_BEG_%d", "SLIDER_END_%d", "SLIDER_HANDLE_%d", "SLIDER_BASE_%d", "SLIDER_KNOB_%d"})
    for (int i = 1; i <= 7; ++i) {
      std::snprintf(name, sizeof name, part, i);
      HideObject(pkg, name);
    }
  for (const char* n : {"UI_COMMON_BACK", "U2_SCROLLER_CORNER", "U2_SCROLLER_CORNER(65)"}) HideObject(pkg, n);
  HideScrollbar(pkg);
  U2LOG("menu: sidebar bar (%.0f,%.0f)-(%.0f,%.0f) from frame (%.0f,%.0f)-(%.0f,%.0f); rows x=%.0f y=%.0f step=%.0f data x=%.0f w=%.0f",
        bar.l, bar.t, bar.r, bar.b, ref.l, ref.t, ref.r, ref.b, startX, startY, f(0x108), dataX, dataW);
}

void* __cdecl Factory(void* showData) {
  if (!g_wantOurs) return g_origFactory(showData);
  g_wantOurs = false;
  void* menu = GameMalloc(0x15C);  // PauseOptionsFNGObject size
  if (!menu) return nullptr;
  UIWidgetMenu_ctor(menu, nullptr, showData);
  *static_cast<void**>(menu) = g_menuVtbl;
  auto m = static_cast<uint8_t*>(menu);
  const char* pkg = *reinterpret_cast<const char**>(m + 4);
  int n = g_cb.rowCount < kMaxRows ? g_cb.rowCount : kMaxRows;
  ApplySidebar(m, pkg, n);

  u2::feng::Print(pkg, u2::feng::HashUpper("Header_text"), g_title.c_str());
  u2::feng::Print(pkg, u2::feng::HashUpper("Header_text2"), g_status.c_str());

  g_rows.clear();
  for (int i = 0; i < n; ++i) {
    void* w = GameMalloc(kWidgetSize);
    if (!w) break;
    FEToggleWidget_ctor(w, nullptr, reinterpret_cast<float*>(m + 0xFC), reinterpret_cast<float*>(m + 0x10C),
                        reinterpret_cast<float*>(m + 0x114), reinterpret_cast<float*>(m + 0x11C), true);
    *static_cast<void**>(w) = g_toggleVtbl;
    RowId(w) = i;
    g_rows.push_back(Row{static_cast<uint8_t*>(w), i, "", ""});
    UIWidgetMenu_AddToggleOption(menu, nullptr, w, true);  // binds OPTION_NAME_n etc. and calls Draw
  }
  UIWidgetMenu_SetInitialOption(menu, nullptr, 0);
  ApplyBacking(pkg);
  g_menu = menu;
  U2LOG("menu: built native screen %p with %zu rows", menu, g_rows.size());
  return menu;
}
}  // namespace

const Layout& GetLayout() { return g_layout; }
bool IsMenuPackage(const char* name) { return g_open && name && _stricmp(name, kPkg) == 0; }

bool Install(const Callbacks& cb, const Layout& layout) {
  g_cb = cb;
  g_layout = layout;
  auto cur = Var<FactoryFn>(kFactorySlot);
  if (reinterpret_cast<uintptr_t>(cur) != 0x4EB650) {
    U2LOG("menu: FNGData[%s] factory is %p, expected 004EB650 (another mod?); native menu disabled", kPkg, cur);
    return false;
  }
  g_origFactory = cur;
  std::memcpy(g_menuVtbl, reinterpret_cast<void*>(0x795CDC), sizeof g_menuVtbl);     // UIWidgetMenu vtable
  g_menuVtbl[2] = reinterpret_cast<void*>(&MenuNotify);
  std::memcpy(g_toggleVtbl, reinterpret_cast<void*>(0x79B3B8), sizeof g_toggleVtbl);  // FEToggleWidget vtable
  g_toggleVtbl[1] = reinterpret_cast<void*>(&ToggleAct);
  g_toggleVtbl[3] = reinterpret_cast<void*>(&ToggleDraw);
  g_toggleVtbl[5] = reinterpret_cast<void*>(&ToggleShow);
  u2::mem::Write<FactoryFn>(kFactorySlot, &Factory);
  U2LOG("menu: factory installed");
  return true;
}

bool Open() {
  if (g_open) return true;
  if (IsFNGLoaded(kPkg)) {
    U2LOG("menu: %s already loaded, not opening", kPkg);
    return false;
  }
  g_wantOurs = true;
  g_wantFrames = 30;
  g_menu = nullptr;
  g_pendingActivate = -1;
  g_pendingBack = false;
  AddFngToUIObject_1(kPkg, nullptr);
  SetMenuKeyboardMap(1);
  Var<uint8_t>(kPauseKeyBlock) = 1;
  g_releasePauseBlockIn = 0;
  g_inputLogIn = 10;
  g_open = true;
  U2LOG("menu: opened (factory ran synchronously: %s)", g_menu ? "yes" : "no");
  return true;
}

// The FE skips a package's pad/keyboard input entirely when its port mask pkg+0x24 is 0
// (ProcessPadsForPackage 0x54D4B0, check at 0x54D4BC; docs/kb/fe-input.md).
void SetHidden(bool hidden) {
  if (!g_open || hidden == g_hidden) return;
  if (auto p = reinterpret_cast<uint8_t*>(u2::feng::FindPackage(kPkg))) {
    auto& mask = *reinterpret_cast<uint32_t*>(p + 0x24);
    if (hidden) {
      g_savedPortMask = mask;
      mask = 0;
    } else {
      mask = g_savedPortMask;
    }
  }
  g_hidden = hidden;
}

bool IsHidden() { return g_open && g_hidden; }

void Close() {
  if (!g_open) return;
  SetHidden(false);
  g_wantOurs = false;
  FEngSetLastButton(kPkg, 0);
  RemoveFngAndJoyHandlers(kPkg);
  SetMenuKeyboardMap(0);
  // Lift the pause-key block a few frames later so an Esc still held from closing can't pause.
  g_releasePauseBlockIn = 3;
  g_open = false;
  g_menu = nullptr;
  g_rows.clear();
  U2LOG("menu: closed");
}

bool IsOpen() { return g_open; }

int CurrentRow() {
  if (!g_menu) return -1;
  void* cur = *reinterpret_cast<void**>(static_cast<uint8_t*>(g_menu) + 0x54);
  return cur ? RowId(cur) : -1;
}

void Tick() {
  if (g_releasePauseBlockIn > 0 && --g_releasePauseBlockIn == 0 && !g_open) Var<uint8_t>(kPauseKeyBlock) = 0;
}

void Update(const std::string& title, const std::string& status) {
  if (g_open && g_inputLogIn > 0 && --g_inputLogIn == 0) {
    auto pkg = reinterpret_cast<uint8_t*>(u2::feng::FindPackage(kPkg));
    auto feng = Var<uint8_t*>(0x8384C4) ? *reinterpret_cast<uint8_t**>(Var<uint8_t*>(0x8384C4) + 8) : nullptr;
    U2LOG("menu input: mapctx=%d menuMap=%d inputsEnabled=%d pkg+1C=%X +24=%X +8C=%X", Var<int>(0x870788),
          Var<uint8_t>(0x870754), feng ? feng[1] : -1, pkg ? *reinterpret_cast<uint32_t*>(pkg + 0x1C) : 0,
          pkg ? *reinterpret_cast<uint32_t*>(pkg + 0x24) : 0, pkg ? *reinterpret_cast<uint32_t*>(pkg + 0x8C) : 0);
  }
  if (g_wantOurs && g_wantFrames > 0 && --g_wantFrames == 0) {
    g_wantOurs = false;
    U2LOG("menu: factory was never called for %s", kPkg);
  }
  if (!g_open || !g_menu) return;
  if (title != g_title) {
    g_title = title;
    u2::feng::Print(kPkg, u2::feng::HashUpper("Header_text"), g_title.c_str());
  }
  if (status != g_status) {
    g_status = status;
    u2::feng::Print(kPkg, u2::feng::HashUpper("Header_text2"), g_status.c_str());
  }
  for (auto& r : g_rows) DrawRow(r.widget, false);
  if (g_pendingActivate >= 0) {
    int row = g_pendingActivate;
    g_pendingActivate = -1;
    g_cb.activate(row);
  }
  if (g_pendingBack) {
    g_pendingBack = false;
    g_cb.back();
  }
}
}  // namespace photomode::menu
