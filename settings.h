#pragma once
#include <string>

#include "u2sdk/optmenu.h"

namespace photomode {
namespace menu = u2::optmenu;  // the native options screen (shared with the daylight tuner)

struct Settings {
  // [PhotoMode]
  int hotkey = 0x23;             // VK_END (user 2026-10-19: Tab may also be the game's message key in free roam)
  bool padCombo = true;          // L3 + R3 opens photo mode
  // [Camera]
  float moveSpeed = 6;           // m/s
  float mouseSensitivity = 0.12f;// degrees per mouse count
  float stickLookSpeed = 110;    // degrees per second at full deflection
  float rollSpeed = 45;          // degrees per second
  bool invertY = false;
  float maxDistance = 30;        // metres from the car
  float fovCalibration = 0;      // 0 = default (see u2math.h)
  // [Capture]
  std::string outputDir;         // default <game>\screenshots
  bool antialias = true;
  int defaultQuality = 1;
  // [Preview]
  bool previewEnable = true;     // live DOF/AA preview once the camera is still
  float previewDelay = 0.15f;    // seconds of stillness before it starts
  // [Menu]
  menu::Layout menuLayout;
  // [Debug]
  bool verbose = false;          // extra diagnostics in the log (cars, menu input, state after exit)

  void Load();
};

}  // namespace photomode
