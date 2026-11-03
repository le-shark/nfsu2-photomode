<#
.SYNOPSIS
  Builds the player release of NFSU2 Photo Mode into build\release\ (a zip plus its folder).
  Never touches the game folder or the INI deployed there: the player INI is this folder's own
  (release\NFSU2PhotoMode.ini, user 2026-10-18: "keep the release one somewhere separate").
.DESCRIPTION
  - Builds the Release configuration (no debug info, no local paths in the binary) and runs the unit tests.
  - -Beta: for a closed beta: the player INI with [Debug] Verbose = 1 (testers' logs carry the detail at once), and
    the user's own README.txt as the only text file (user 2026-10-19: the user writes the player texts).
.EXAMPLE
  .\release\make_release.ps1
  .\release\make_release.ps1 -Beta
#>
param([switch]$Beta)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path "$here\..").Path
$out = Join-Path $repo 'build\release'

& "$repo\build.ps1" -Config Release -Test | Out-Host
$bin = Join-Path $repo 'build\Release\bin'

# u2sdk as build.ps1 finds it: the submodule, or the scripts workspace's checkout.
$u2 = Join-Path $repo 'external\u2sdk'
if (-not (Test-Path "$u2\CMakeLists.txt")) { $u2 = (Resolve-Path "$repo\..\..\common\u2sdk").Path }

# Third-party notices: MinHook's licence asks for its notice in binary releases; stb_image_write is public domain / MIT.
$notices = Join-Path $env:TEMP 'nfsu2_photomode_notices.txt'
@(
  'This mod contains the following third-party code.', '',
  '== MinHook (https://github.com/TsudaKageyu/minhook), BSD 2-Clause License ==', '',
  (Get-Content -Raw "$u2\third_party\minhook\LICENSE.txt"), '',
  '== stb_image_write (https://github.com/nothings/stb), public domain (or MIT, at your choice) ==', '',
  'Copyright (c) 2017 Sean Barrett. Used under the public domain dedication (www.unlicense.org).'
) -join "`r`n" | Set-Content -Encoding utf8 $notices

$asi = "$bin\NFSU2PhotoMode.asi"
# No developer paths in a release binary (RelWithDebInfo embeds the PDB path, 2026-10-18 review).
$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($asi))
if ($text -match 'scripts_workspace|\.pdb') { throw "$asi contains a local path or a PDB reference" }

$ver = (Select-String -Path "$repo\version.h" -Pattern '#define PHOTOMODE_VERSION "(.+)"').Matches[0].Groups[1].Value
$name = "NFSU2PhotoMode-$ver"
$d = Join-Path $out $name
if (Test-Path $d) { Remove-Item -Recurse -Force $d }
New-Item -ItemType Directory -Force "$d\scripts" | Out-Null
Copy-Item $asi, "$here\NFSU2PhotoMode.ini" "$d\scripts\"
Copy-Item "$here\README.txt" $d
if ($Beta) {
  $ini = "$d\scripts\NFSU2PhotoMode.ini"
  $iniText = (Get-Content -Raw $ini) -replace '(?m)^Verbose = 0(\s)', 'Verbose = 1$1'
  if ($iniText -notmatch '(?m)^Verbose = 1') { throw 'beta: Verbose not found in the player INI' }
  Set-Content -NoNewline -Encoding ascii $ini $iniText
} else { Copy-Item $notices "$d\THIRD-PARTY-NOTICES.txt" }
$zip = "$d.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$d\*" -DestinationPath $zip
Write-Host "$name.zip: $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB -> $out"
