#pragma once
// The front-end garage as a photo-mode scene (the crib, the shops, the car lot, the showroom: wherever GarageMain draws
// its platform set). Research: docs/research/2026-11-02-photomode-garage-postrace.md.
#include "u2sdk/u2math.h"

namespace photomode::garage {

// In the front end (flow state 3) with GarageMain.fng up and its set drawn.
bool Present();
// Where the camera may go: the set's BACKDROP model box (the set is drawn at the origin with no rotation), a little
// inside its walls and roof, and above the floor. False without one (the camera then has only its distance limit).
bool CameraBox(u2::Vec3* mn, u2::Vec3* mx);
// The particle step call that GarageMain makes for its set's emitters (the Showroom's smoke) with real time.
bool IsSetEmitters(const void* particleManager);
// The turntable's own input switch off while photo mode is open (true) and back as it was (false). Its orbit events
// come from the joystick layer, not the FE, so the menu's input masks do not reach them ([game] 2026-11-03, `.net`:
// XtendedInput sends them for A / D and the right stick).
void HoldTurntable(bool hold);

}  // namespace photomode::garage
