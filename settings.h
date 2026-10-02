#pragma once
#include <string>

#include "native_menu.h"

namespace photomode {

struct Settings {
  // [PhotoMode]
  int hotkey = 0x09;             // VK_TAB
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
  // [Menu]
  menu::Layout menuLayout;
  // [Debug]
  bool verbose = false;          // extra diagnostics in the log (cars, menu input, state after exit)

  void Load();
};

}  // namespace photomode
