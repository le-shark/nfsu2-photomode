#include <windows.h>

#include "u2sdk/crash.h"
#include "u2sdk/game.h"
#include "u2sdk/log.h"
#include "version.h"

namespace photomode { bool Install(); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  u2::log::Init();
  U2LOG("NFSU2 Photo Mode " PHOTOMODE_VERSION " (built " __DATE__ " " __TIME__ ")");
  u2::crash::InstallLogger();
  if (!u2::IsSupportedExe()) {
    U2LOG("unsupported executable (need NFSU2 v1.2 SPEED2.EXE); staying inactive");
    return TRUE;
  }
  U2LOG(photomode::Install() ? "installed" : "install failed");
  return TRUE;
}
