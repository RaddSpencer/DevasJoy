# Builds the recomp and gathers a folder that can be copied anywhere and run: release\AsurasWrathRecomp\
#   asura.exe, asura_setup.exe       the recomp and its setup
#   avcodec/avutil/swresample DLLs   FFmpeg (LGPL), the XMA audio decoder
#   msvcp140/vcruntime140 DLLs       the Visual C++ runtime, deployed beside the exes (allowed by Microsoft's redistribution terms)
#   shader_manifest.txt              the record of shaders the game uses (no game code: keys into the player's own files), compiled on first start
#   README.txt, asura_debug.cmd (starts with a log), licenses\
#   and release\AsurasWrathRecomp.zip of all that, to hand out
# No game files or anything made from them go in: the player's own copy is unpacked by asura_setup.exe into this folder (game\, private\).
# Not included: assets\prompts (PlayStation / Switch button prompts made from the game's own icons by tools\make_prompts.ps1).
#
# usage: powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 [-NoBuild] [-Out <folder>]
param([switch]$NoBuild, [string]$Out = "")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $repo 'release\AsurasWrathRecomp' }
$build = Join-Path $repo 'host\build'

if (-not $NoBuild) {
    & cmd /c "`"$repo\host\build.cmd`""
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (Test-Path $Out) {
    # keep a player's unpacked files if the release folder was used for a test install; replace only what this script puts there
    Get-ChildItem $Out -File | Where-Object { $_.Name -match '\.(exe|dll|txt|cmd)$' } | Remove-Item -Force
    if (Test-Path "$Out\licenses") { Remove-Item -Recurse -Force "$Out\licenses" }
} else { New-Item -ItemType Directory -Force $Out | Out-Null }

Copy-Item "$build\asura.exe", "$build\asura_setup.exe" $Out
Copy-Item "$build\avcodec-*.dll", "$build\avutil-*.dll", "$build\swresample-*.dll" $Out
Copy-Item "$repo\shader_manifest.txt" $Out

# Visual C++ runtime: the newest x64 CRT folder of the installed Visual Studio
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "Visual C++ runtime (VC\Redist\MSVC\...\x64\Microsoft.VC*.CRT) not found" }
Copy-Item "$($crt.FullName)\msvcp140.dll", "$($crt.FullName)\vcruntime140.dll", "$($crt.FullName)\vcruntime140_1.dll" $Out

New-Item -ItemType Directory -Force "$Out\licenses" | Out-Null
Copy-Item "$repo\thirdparty\ffmpeg\LICENSE.txt" "$Out\licenses\FFmpeg-LICENSE.txt"
Copy-Item "$repo\thirdparty\libmspack\COPYING.LIB" "$Out\licenses\libmspack-COPYING.LIB.txt"
Copy-Item "$repo\thirdparty\xexpatch\LICENSE-XenonRecomp.md" "$Out\licenses\XenonRecomp-LICENSE.md"
Copy-Item "$repo\thirdparty\xexpatch\unlicense.txt" "$Out\licenses\tiny-AES-c-unlicense.txt"

@"
Asura's Wrath - PC recompilation (test build)
=============================================

You need your own copy of the Xbox 360 game. Nothing from the game is included here.

Requirements
  - Windows 10 or 11, 64-bit. ("N" editions of Windows need the Media Feature Pack.)
  - A graphics card with Direct3D 11 (feature level 11.0 or newer) and a current driver.
  - An Xbox-compatible controller (XInput). Keyboard and mouse are not supported yet.
    PlayStation and other controllers work through Steam Input or a similar tool.
  - About 11 GB of free space with all DLC (6.2 GB without).
  - Put this folder somewhere you can write to, e.g. C:\Games\AsurasWrath. Not in Program Files.
    A path with only plain English letters is safest.

Windows may say "Windows protected your PC" the first time (these programs aren't signed):
choose "More info", then "Run anyway". Some antivirus programs flag new unsigned programs too.

Setup
1. Run asura_setup.exe (or asura.exe, which opens setup the first time).
2. Give it your files: drop them on its window, use Browse..., or type the paths.
     - Asura's Wrath disc image (.iso)        required
     - Title update 1 (TU_11K620N_...)         required
     - Episode Pack: Part IV                   strongly recommended (episodes 19-22, the real ending)
     - Episode 11.5, Episode 15.5, Lost Episode 1, Lost Episode 2   optional
   Each file is checked against the known SHA-256: green is good, red is missing or a different version.
   OK unpacks them into this folder.
3. Start asura.exe. The first start reads the game files and compiles shaders for a few minutes,
   with a progress screen. Later starts go straight in.

Run asura_setup.exe again at any time to add DLC.

Playing
  - Options (resolution, frame rate, Story Mode, vsync...): press Back/View on the pad or F1 on the title screen.
  - Alt+Enter or F11 switches between a window and full screen.

If something goes wrong
  - If the game crashes or stops with an error, it saves a report in the logs folder and says so.
  - To record a log, start asura_debug.cmd instead of asura.exe. The log is saved in the logs folder.
  - Please send the files from the logs folder with a short description of what happened.
"@ | Set-Content -Encoding utf8 "$Out\README.txt"

@"
@echo off
rem Starts the game with a log (logs\asura_<date>.log), for reporting problems.
cd /d "%~dp0"
set ASURA_SHOTRING=0
start "" "%~dp0asura.exe" --log
"@ | Set-Content -Encoding ascii "$Out\asura_debug.cmd"

# the zip to hand out: everything above, no game data, no logs or caches from a test install
$zip = Join-Path (Split-Path -Parent $Out) 'AsurasWrathRecomp.zip'
if (Test-Path $zip) { Remove-Item -Force $zip }
$items = Get-ChildItem $Out | Where-Object { $_.Name -notin @('game', 'private', 'cache', 'shadercache', 'saves', 'logs', 'screenshots', 'asura.ini') }
Compress-Archive -Path $items.FullName -DestinationPath $zip -CompressionLevel Optimal
"Zip: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"

$size = (Get-ChildItem $Out -Recurse -File | Where-Object { $_.FullName -notmatch '\\(game|private|cache|shadercache|saves|logs)\\' } | Measure-Object Length -Sum).Sum
"Release folder: $Out ($([math]::Round($size / 1MB)) MB without game data)"
Get-ChildItem $Out -Name
