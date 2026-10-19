#include "settings.h"

#include <windows.h>

#include "u2sdk/ini.h"
#include "u2sdk/log.h"

namespace photomode {
namespace {
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
  if (int vk = u2::ParseVirtualKey(ini.GetString("PhotoMode", "Hotkey", "END"))) hotkey = vk;
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
  // Absolute: a drive ("D:\..."), a network share ("\\nas\...") or a root ("\shots"); anything else is under the game.
  else if (!(outputDir.size() >= 2 && outputDir[1] == ':') && outputDir[0] != '\\' && outputDir[0] != '/')
    outputDir = GameDir() + outputDir;
  antialias = ini.GetBool("Capture", "AntiAliasing", antialias);
  defaultQuality = ini.GetInt("Capture", "DefaultQuality", defaultQuality);

  previewEnable = ini.GetBool("Preview", "Enable", previewEnable);
  previewDelay = ini.GetFloat("Preview", "Delay", previewDelay);

  menuLayout.width = ini.GetFloat("Menu", "Width", menuLayout.width);
  menuLayout.margin = ini.GetFloat("Menu", "Margin", menuLayout.margin);
  menuLayout.valueWidth = ini.GetFloat("Menu", "ValueWidth", menuLayout.valueWidth);
  menuLayout.backingAlpha = ini.GetInt("Menu", "BackingAlpha", menuLayout.backingAlpha);

  verbose = ini.GetBool("Debug", "Verbose", verbose);
  U2LOG("settings: hotkey=0x%02X padCombo=%d move=%.1f sens=%.3f maxDist=%.0f out=%s", hotkey, padCombo, moveSpeed,
        mouseSensitivity, maxDistance, outputDir.c_str());
}
}  // namespace photomode
