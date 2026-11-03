<#
.SYNOPSIS
  Build NFSU2 Photo Mode (x86, MSVC 2022, Ninja) and optionally run its unit tests / deploy it.
.DESCRIPTION
  u2sdk comes from the submodule (external\u2sdk, git submodule update --init). Inside the NFSU2 scripts workspace,
  where this repo sits at scripts\photomode and the submodule is left empty, the workspace's common\u2sdk is used.
.EXAMPLE
  .\build.ps1 -Test
  .\build.ps1 -Test -Deploy -GameDir "D:\Games\NFSU2"   # copies the .asi (and the .ini if missing) to <GameDir>\scripts
#>
param(
  [ValidateSet('Release', 'RelWithDebInfo', 'Debug')] [string]$Config = 'RelWithDebInfo',
  [switch]$Test,
  [switch]$Deploy,
  [switch]$Clean,
  [string]$GameDir
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root "build\$Config"

$u2 = Join-Path $root 'external\u2sdk'
if (-not (Test-Path "$u2\CMakeLists.txt")) {
  $wsU2 = Join-Path $root '..\..\common\u2sdk'
  if (-not (Test-Path "$wsU2\CMakeLists.txt")) { throw 'u2sdk not found: run git submodule update --init' }
  $u2 = (Resolve-Path $wsU2).Path
  # The submodule pins a u2sdk commit: a build from another checkout matches this repo only at that commit.
  $pin = ((git -C $root ls-tree HEAD external/u2sdk) -split '\s+')[2]
  $head = git -C $u2 rev-parse HEAD
  $dirty = git -C $u2 status --porcelain
  if ($pin -ne $head -or $dirty) {
    Write-Warning "u2sdk at $u2 is not the pinned commit $pin (it is at $head$(if ($dirty) { ', with uncommitted changes' }))"
  }
}
. (Join-Path $u2 'tools\msvc.ps1')

# Sources must be plain ASCII: Python edits twice wrote C escapes such as \x55 as UTF-8 characters, which turns a
# hook's entry-byte check into a silent mismatch.
$bad = Get-ChildItem $root -Recurse -Include *.cpp, *.h, *.hlsl |
  Where-Object { $_.FullName -notmatch '\\(build|external)\\' } |
  Where-Object { [IO.File]::ReadAllBytes($_.FullName) | Where-Object { $_ -gt 127 } | Select-Object -First 1 }
if ($bad) { throw "Non-ASCII bytes in: $($bad.FullName -join ', ')" }

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DU2SDK_DIR=$($u2 -replace '\\', '/')" | Out-Host
if ($LASTEXITCODE) { throw 'CMake configure failed' }
cmake --build $build | Out-Host
if ($LASTEXITCODE) { throw 'Build failed' }

if ($Test) {
  & (Join-Path $build 'bin\photomode_tests.exe') | Out-Host
  if ($LASTEXITCODE) { throw 'Unit tests failed' }
}

if ($Deploy) {
  if (-not $GameDir) {
    # Inside the scripts workspace the game folder is three levels up (<game>\scripts_workspace\scripts\photomode).
    $GameDir = Join-Path $root '..\..\..'
    if (-not (Test-Path (Join-Path $GameDir 'SPEED2.EXE'))) { throw 'pass -GameDir (the folder with SPEED2.EXE)' }
  }
  $scripts = Join-Path $GameDir 'scripts'
  New-Item -ItemType Directory -Force $scripts | Out-Null
  Copy-Item (Join-Path $build 'bin\NFSU2PhotoMode.asi') $scripts -Force
  Write-Host "deployed NFSU2PhotoMode.asi -> $scripts"
  # The INI is copied only if the player doesn't already have one (keeps their settings).
  $ini = Join-Path $scripts 'NFSU2PhotoMode.ini'
  if (-not (Test-Path $ini)) { Copy-Item (Join-Path $root 'NFSU2PhotoMode.ini') $ini; Write-Host 'deployed NFSU2PhotoMode.ini' }
  else { Write-Host 'kept existing NFSU2PhotoMode.ini' }
}
