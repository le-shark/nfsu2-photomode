#include "settings.h"

#include <windows.h>

#include <cstdlib>

#include "u2sdk/ini.h"
#include "u2sdk/log.h"

namespace photomode {
namespace {
uint32_t Color(const u2::Ini& ini, const char* key, uint32_t def) {
  std::string s = ini.GetString("UI", key, "");
  return s.empty() ? def : static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 16));
}
std::string GameDir() {
  char buf[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string p = buf;
  return p.substr(0, p.find_last_of("\\/") + 1);
}
}  // namespace

void Settings::Load() {
  u2::Ini ini;
  U2LOG("settings: %s", ini.Path().c_str());
  if (int vk = u2::ParseVirtualKey(ini.GetString("PhotoMode", "Hotkey", "TAB"))) hotkey = vk;
  padCombo = ini.GetBool("PhotoMode", "GamepadCombo", padCombo);

  moveSpeed = ini.GetFloat("Camera", "MoveSpeed", moveSpeed);
  mouseSensitivity = ini.GetFloat("Camera", "MouseSensitivity", mouseSensitivity);
  stickLookSpeed = ini.GetFloat("Camera", "StickLookSpeed", stickLookSpeed);
  rollSpeed = ini.GetFloat("Camera", "RollSpeed", rollSpeed);
  invertY = ini.GetBool("Camera", "InvertY", invertY);
  maxDistance = ini.GetFloat("Camera", "MaxDistance", maxDistance);
  fovCalibration = ini.GetFloat("Camera", "FovCalibration", fovCalibration);

  outputDir = ini.GetString("Capture", "OutputDir", "");
  if (outputDir.empty()) outputDir = GameDir() + "screenshots";
  else if (outputDir.size() < 2 || outputDir[1] != ':') outputDir = GameDir() + outputDir;
  antialias = ini.GetBool("Capture", "AntiAliasing", antialias);
  defaultQuality = ini.GetInt("Capture", "DefaultQuality", defaultQuality);

  layout.x = ini.GetFloat("UI", "X", layout.x);
  layout.y = ini.GetFloat("UI", "Y", layout.y);
  layout.lineHeight = ini.GetFloat("UI", "LineHeight", layout.lineHeight);
  layout.valueOffset = ini.GetFloat("UI", "ValueOffset", layout.valueOffset);
  layout.helpY = ini.GetFloat("UI", "HelpY", layout.helpY);
  layout.colorNormal = Color(ini, "ColorNormal", layout.colorNormal);
  layout.colorSelected = Color(ini, "ColorSelected", layout.colorSelected);
  layout.colorTitle = Color(ini, "ColorTitle", layout.colorTitle);
  layout.colorHelp = Color(ini, "ColorHelp", layout.colorHelp);

  verbose = ini.GetBool("Debug", "Verbose", verbose);
  U2LOG("settings: hotkey=0x%02X padCombo=%d move=%.1f sens=%.3f maxDist=%.0f out=%s", hotkey, padCombo, moveSpeed,
        mouseSensitivity, maxDistance, outputDir.c_str());
}
}  // namespace photomode
