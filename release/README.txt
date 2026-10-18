NFSU2 Photo Mode
================

A photo mode for Need for Speed Underground 2 races: the world stops, the HUD hides, and you get a free camera with
depth of field, motion blur and anti-aliased photos up to twice the screen resolution.


Requirements
------------
- Need for Speed Underground 2, version 1.2 (SPEED2.EXE). Other versions: the mod stays off.
- An ASI loader, e.g. Ultimate ASI Loader (dinput8.dll; it comes with ThirteenAG's Widescreen Fix).


Install
-------
Copy the "scripts" folder into the game folder (next to SPEED2.EXE).


Controls
--------
Open / close        Tab (pad: click both sticks, L3 + R3). Esc or pad B also closes it.
Menu                arrows, Enter, Esc (pad: D-pad, A, B). H (pad Y) hides the menu.
Camera              WASD move, Q / E down / up, mouse look, Z / C roll, mouse wheel focal length,
                    Shift faster, Ctrl slower, [ / ] orbit another racer
Pad camera          left stick move, right stick look, LT / RT down / up, LB / RB roll
Take a picture      Space (pad X); Tab, Esc or pad B cancel a picture in progress

The menu sets the camera (orbit or free), focus (Center, Closest, Aim, Manual), aperture, shutter (motion blur, and
whether the blur follows the car), quality and resolution (1x or 2x the screen). With the camera still, the screen
shows the finished look before you take the picture.

Pictures are saved as PNG in the game's "screenshots" folder ([Capture] OutputDir in the INI).


Settings
--------
scripts\NFSU2PhotoMode.ini: the key, mouse and stick speeds, the camera's range around the car, the output folder,
the default quality and the menu's size.


Compatibility
-------------
- Works with and without Widescreen Fix, NFSU2 Unlimiter, NFSU2 Daylight and Extra Options.
- Not available in online races (the game cannot pause there).
- ReShade effects are not in the pictures (photos are taken before ReShade draws).


Uninstall
---------
Delete scripts\NFSU2PhotoMode.asi and scripts\NFSU2PhotoMode.ini. The mod changes no game files.


Problems
--------
The mod writes scripts\NFSU2PhotoMode.log (new each start). Set [Debug] Verbose = 1 in the INI for more detail and
send the log with a description.


Credits
-------
Third-party code: see THIRD-PARTY-NOTICES.txt.
