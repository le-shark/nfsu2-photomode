// "Photo Mode" icon in the game's pause menu (user 2026-10-20), after Options and before Replay.
//
// - The pause menu (UI_Pause.fng, PauseFNGObject vtable 0x797CB0) is an IconScrollerMenu; Setup 0x4D9300 adds its
//   icons through AddOption 0x50DAB0 (slot 6). In 0x4C2900 Options is added (option vtable 0x795510, 0x4C2A66) only
//   when 0x4A7BF0 allows it, then Replay (0x4C2AB4, built by 0x4C2640). We add our IconOption right after Options,
//   as the flight sim adds its Quick Race icon after Free Run (scripts/flightsim/qrentry.cpp, [game] 2026-10-16) and
//   the career hub builds its icons (scripts/career/hub.cpp, [game]): game malloc 0x575620(0x48), IconOption ctor
//   0x51F670 thiscall(tex, name, 0), our vtable {0x4AEB70 dtor, React}.
// - Icon: ICON_SHOWCASE 0xD1D9D8A4 (bStringHash), in GLOBAL\GLOBALB.BUN, which stays loaded for the whole game.
// - Caption: RefreshHeader 0x50DA50 writes ICON_TITLE (+0x134) from the option's language hash; there is no "Photo
//   Mode" string, so we print it after the original (qrentry.cpp RefreshDetour).
// - React: on select (0xC407210) we run the Resume Race option's own React 0x4C24C0 (option vtable 0x7954D0:
//   RaceCoordinator [0x8900D8] post 0x5F4240(1), FEngSetLastButton, RemoveFNGFromUIObject), so the game unpauses
//   exactly as Resume does, and ask photo mode to open once the game is no longer paused (TakeRequest).
#include "pauseentry.h"

#include <cstdint>
#include <cstring>

#include "u2sdk/game.h"
#include "u2sdk/hook.h"
#include "u2sdk/log.h"

namespace photomode::pause {
namespace {
using u2::Var;

template <typename F>
F Fn(uintptr_t a) { return reinterpret_cast<F>(a); }

constexpr uintptr_t kAddOption = 0x50DAB0, kRefreshHeader = 0x50DA50;
constexpr uintptr_t kPauseVtbl = 0x797CB0, kOptionsOptVtbl = 0x795510, kResumeReact = 0x4C24C0;
constexpr uint32_t kMsgSelect = 0xC407210;
constexpr uint32_t kIconShowcase = 0xD1D9D8A4;
constexpr uint32_t kScShowcase = 0x535A2751;  // SC_SHOWCASE "ShowCase"; the caption is printed by RefreshDetour

auto GameMalloc = Fn<void*(__cdecl*)(size_t)>(0x575620);
auto IconOption_ctor = Fn<void(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t)>(0x51F670);
auto FEngPrintf = Fn<void(__cdecl*)(void*, const char*, ...)>(0x537BE0);

using AddOptionFn = int(__fastcall*)(void*, void*, void*);
using RefreshFn = void(__fastcall*)(void*, void*);
using ReactFn = void(__fastcall*)(void*, void*, const char*, uint32_t, void*, uint32_t, uint32_t);
AddOptionFn oAddOption = nullptr;
RefreshFn oRefresh = nullptr;
bool g_request = false;

void __fastcall PhotoReact(void* opt, void* edx, const char* pkg, uint32_t msg, void* obj, uint32_t p1, uint32_t p2) {
  if (msg != kMsgSelect) return;
  U2LOG("pause menu: Photo Mode selected; resuming the race");
  g_request = true;
  Fn<ReactFn>(kResumeReact)(opt, edx, pkg, msg, obj, p1, p2);
}
void* g_vtbl[2] = {reinterpret_cast<void*>(0x4AEB70), reinterpret_cast<void*>(&PhotoReact)};

bool IsOurs(void* opt) { return opt && *static_cast<void**>(opt) == static_cast<void*>(g_vtbl); }
void* CurrentOption(void* menu) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + 0x58); }

int __fastcall AddOptionDetour(void* menu, void* edx, void* opt) {
  // Read before the call: AddOption may free the option when there is no slot.
  bool afterOptions = opt && menu && *static_cast<uintptr_t*>(opt) == kOptionsOptVtbl &&
                      *static_cast<uintptr_t*>(menu) == kPauseVtbl;
  int r = oAddOption(menu, edx, opt);
  if (afterOptions && u2::IsInGame() && !u2::IsNetworkSession()) {
    if (void* o = GameMalloc(0x48)) {
      IconOption_ctor(o, nullptr, kIconShowcase, kScShowcase, 0);
      *static_cast<void**>(o) = g_vtbl;
      AddOptionFn add = *reinterpret_cast<AddOptionFn*>(*static_cast<uint8_t**>(menu) + 0x18);  // slot 6, as 0x4C2A7D
      int s = add(menu, nullptr, o);
      U2LOG("pause menu: Photo Mode icon %s", s ? "added" : "NO SLOT (deleted by the game)");
    }
  }
  return r;
}

void __fastcall RefreshDetour(void* menu, void* edx) {
  oRefresh(menu, edx);
  if (!menu || *static_cast<uintptr_t*>(menu) != kPauseVtbl) return;
  void* label = *reinterpret_cast<void**>(static_cast<uint8_t*>(menu) + 0x134);
  if (IsOurs(CurrentOption(menu)) && label) FEngPrintf(label, "%s", "Photo Mode");
}

bool EntryIs(uintptr_t at, const char* bytes, size_t n) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(at);
  if (*p == 0xE9) return true;  // another of our ASIs hooked it first (MinHook chains; lessons.md section 1)
  return std::memcmp(p, bytes, n) == 0;
}
}  // namespace

bool Install() {
  bool add = EntryIs(kAddOption, "\x8B\x41\x4C\x56\x8B\x74\x24\x08", 8);
  bool ref = EntryIs(kRefreshHeader, "\x56\x8B\xF1\x8B\x46\x58\x85\xC0", 8);
  bool opt = *reinterpret_cast<const uintptr_t*>(kOptionsOptVtbl + 4) == 0x4C2700 &&
             *reinterpret_cast<const uintptr_t*>(0x7954D0 + 4) == kResumeReact;
  if (!add || !ref || !opt) {
    U2LOG("pause menu: code differs (AddOption %d, RefreshHeader %d, option vtables %d); no Photo Mode icon", add, ref, opt);
    return false;
  }
  if (!u2::hook::Create(kAddOption, &AddOptionDetour, &oAddOption, "AddOption 0x50DAB0") ||
      !u2::hook::Create(kRefreshHeader, &RefreshDetour, &oRefresh, "RefreshHeader 0x50DA50"))
    return false;
  return true;
}

bool TakeRequest() {
  bool r = g_request;
  g_request = false;
  return r;
}
}  // namespace photomode::pause
