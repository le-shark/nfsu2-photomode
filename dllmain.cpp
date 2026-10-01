#include <windows.h>

#include "u2sdk/game.h"
#include "u2sdk/log.h"

namespace photomode { bool Install(); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  u2::log::Init();
  U2LOG("NFSU2 Photo Mode " __DATE__);
  if (!u2::IsSupportedExe()) {
    U2LOG("unsupported executable (need NFSU2 v1.2 SPEED2.EXE); staying inactive");
    return TRUE;
  }
  U2LOG(photomode::Install() ? "installed" : "install failed");
  return TRUE;
}
