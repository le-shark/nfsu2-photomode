# NFSU2 Photo Mode

A photo mode for Need for Speed Underground 2 (v1.2, `SPEED2.EXE`) as an ASI plugin: a free and orbit camera,
lenses with depth of field and motion blur, supersampled photos, grid, aspect ratios and watermarks, in a menu that
looks like the game's own.

## Build

Needs Visual Studio 2022 (or its Build Tools) with the C++ x86 toolset and the Windows SDK (for `fxc`). NFSU2 is
32-bit.

```powershell
git clone --recursive <this repo>      # or: git submodule update --init
.\build.ps1 -Test                      # builds build\RelWithDebInfo\bin\NFSU2PhotoMode.asi and runs the tests
.\build.ps1 -Deploy -GameDir "D:\Games\NFSU2"
.\release\make_release.ps1             # the player package in build\release
```

Shared code comes from [u2sdk](../nfsu2-u2sdk) (submodule in `external/u2sdk`).

## Licence

MIT (see `LICENSE`). Third-party code keeps its own licence: MinHook (BSD 2-Clause) and stb_image_write (public
domain or MIT), both in u2sdk.
