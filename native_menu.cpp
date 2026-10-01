#include "native_menu.h"

#include <windows.h>

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
auto FEToggleWidget_BlinkArrows = Fn<void(__fastcall*)(void*, void*, unsigned)>(0x51E6B0);
auto FEngPrintf = Fn<void(__cdecl*)(void*, const char*, ...)>(0x537BE0);               // Unlimiter InGameFunctions.h:135
auto AddFngToUIObject_1 = Fn<void(__cdecl*)(const char*, void*)>(0x555E80);            // push + AddJoyHandlers
auto RemoveFngAndJoyHandlers = Fn<void(__cdecl*)(const char*)>(0x558F00);              // what the game pairs with it
auto RemoveFNGFromUIObject = Fn<void(__cdecl*)(const char*)>(0x5379A0);
auto IsFNGLoaded = Fn<bool(__cdecl*)(const char*)>(0x52CF60);
auto GetHelpBarFNGName = Fn<const char*(__cdecl*)()>(0x50CEA0);
auto FEngShowPackage = Fn<void(__cdecl*)(const char*, const char*, unsigned, int)>(0x555F10);
auto FEngSetLastButton = Fn<void(__cdecl*)(const char*, int)>(0x50B8C0);
auto HelpBar_Show = Fn<void(__fastcall*)(void*, void*)>(0x54E6A0);
// Selects the menu keyboard action map while racing (writes byte 0x870754, which the per-frame map
// selector 0x5CFBF0 checks). The game does the same for the in-world dyno screen (0x557AEE).
auto SetMenuKeyboardMap = Fn<void(__cdecl*)(char)>(0x5B7ED0);
auto HelpBar_SyncByMask = Fn<void(__fastcall*)(void*, void*, unsigned, const char*)>(0x54E6E0);
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
std::string g_title, g_status;

void* g_menuVtbl[5];
void* g_toggleVtbl[17];

constexpr size_t kWidgetSize = 0x58;  // 0x54 FEToggleWidget + our row id (0x58 variants exist in-game)
int& RowId(void* w) { return *reinterpret_cast<int*>(static_cast<uint8_t*>(w) + 0x54); }
void* LabelString(void* w) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(w) + 0x2C); }
void* ValueString(void* w) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(w) + 0x30); }

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

// Act: same shape as the game's toggle rows (e.g. 0x4C3C70): change value, BlinkArrows, Draw.
void __fastcall ToggleAct(void* w, void*, const char* /*pkg*/, unsigned msg) {
  if (msg == kMsgLeft || msg == kMsgRight) {
    g_cb.adjust(RowId(w), msg == kMsgRight ? 1 : -1);
    FEToggleWidget_BlinkArrows(w, nullptr, msg);
    DrawRow(w, true);
  } else if (msg == kMsgSelect) {
    g_pendingActivate = RowId(w);
  }
}

// Screen message handler: base UIWidgetMenu first (navigation, LEFT/RIGHT -> row Act, tick).
void __fastcall MenuNotify(void* self, void*, unsigned msg, void* obj, int p1, int p2) {
  UIWidgetMenu_Notify(self, nullptr, msg, obj, p1, p2);
  if (msg == kMsgAccept) {
    void* cur = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x54);
    if (cur) g_pendingActivate = RowId(cur);
  } else if (msg == kMsgBack) {
    g_pendingBack = true;  // never tear down from inside the FE update
  }
}

void PushHelpBar(const char* ownPkg) {
  // Same sequence as the UI_Pause ctor (0x4EB489..0x4EB4CD); mask 0x10400 = one line + Back.
  const char* bar = GetHelpBarFNGName();
  if (!IsFNGLoaded(bar)) {
    FEngShowPackage(bar, ownPkg, 0xFF, 0);
    void* hb = Var<void*>(0x8384D8);
    if (hb) {
      HelpBar_Show(hb, nullptr);
      HelpBar_SyncByMask(hb, nullptr, 0x10400, ownPkg);
    }
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

void* __cdecl Factory(void* showData) {
  if (!g_wantOurs) return g_origFactory(showData);
  g_wantOurs = false;
  void* menu = GameMalloc(0x15C);  // PauseOptionsFNGObject size
  if (!menu) return nullptr;
  UIWidgetMenu_ctor(menu, nullptr, showData);
  *static_cast<void**>(menu) = g_menuVtbl;
  auto m = static_cast<uint8_t*>(menu);
  *reinterpret_cast<int*>(m + 0x134) = 8;  // visible rows, as PauseOptions
  const char* pkg = *reinterpret_cast<const char**>(m + 4);

  u2::feng::Print(pkg, u2::feng::HashUpper("Header_text"), g_title.c_str());
  u2::feng::Print(pkg, u2::feng::HashUpper("Header_text2"), g_status.c_str());

  g_rows.clear();
  int n = g_cb.rowCount < kMaxRows ? g_cb.rowCount : kMaxRows;
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
  PushHelpBar(pkg);
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

void Close() {
  if (!g_open) return;
  g_wantOurs = false;
  FEngSetLastButton(kPkg, 0);
  const char* bar = GetHelpBarFNGName();
  if (IsFNGLoaded(bar)) RemoveFNGFromUIObject(bar);
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
