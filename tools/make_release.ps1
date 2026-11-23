# Builds the recomp and gathers a package that can be handed out and run anywhere:
#   release\AsurasWrathRecomp-<version>\        the folder
#   release\AsurasWrathRecomp-<version>.zip     the same, zipped, to send
#   release\AsurasWrathRecomp-<version>.zip.sha256.txt
# Contents:
#   asura.exe, asura_setup.exe       the recomp and its setup, stamped with the version and commit (host\version.h)
#   avcodec/avutil/swresample DLLs   FFmpeg (LGPL), the XMA audio decoder
#   msvcp140/vcruntime140 DLLs       the Visual C++ runtime, deployed beside the exes (allowed by Microsoft's redistribution terms)
#   shader_manifest.txt              the record of shaders the game uses (no game code: keys into the player's own files), compiled on first start
#   README.txt, BETA_TESTING.txt, asura_debug.cmd (starts with a log), licenses\
# No game files or anything made from them go in: the player's own copy is unpacked by asura_setup.exe into that folder (game\, private\).
# Not included: assets\prompts (PlayStation / Switch button prompts made from the game's own icons by tools\make_prompts.ps1).
#
# usage: powershell -ExecutionPolicy Bypass -File tools\make_release.ps1 [-Version 0.1.0-beta.1] [-AllowDirty] [-NoBuild] [-Out <folder>]
#   It refuses to build from uncommitted changes to tracked files (the package must match a commit) unless -AllowDirty (the commit is then
#   marked "-dirty"). -NoBuild packages host\build as it is, without the version stamp.
param([string]$Version = "0.1.0-beta.1", [switch]$AllowDirty, [switch]$NoBuild, [string]$Out = "")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$name = "AsurasWrathRecomp-$Version"
if (-not $Out) { $Out = Join-Path $repo "release\$name" }
$build = Join-Path $repo 'host\build'

Push-Location $repo
try {
    $commit = (git rev-parse --short HEAD).Trim()
    $dirty = (git status --porcelain --untracked-files=no) -ne $null
} finally { Pop-Location }
if ($dirty) {
    if (-not $AllowDirty) { throw "Uncommitted changes to tracked files: commit them first, or pass -AllowDirty (the package would not match commit $commit)." }
    $commit += "-dirty"
}

if (-not $NoBuild) {
    # stamp the version for this build only (host\version.h); removed again afterwards so ordinary builds say "dev"
    $stamp = Join-Path $repo 'host\generated\build_version.h'
    "// written by tools\make_release.ps1; removed after the release build`n#define ASURA_VERSION `"$Version`"`n#define ASURA_COMMIT `"$commit`"" |
        Set-Content -Encoding ascii $stamp
    # version.h includes the stamp only if it exists, which Ninja's dependency lists don't see: touch version.h so everything using it rebuilds,
    # now and again after the stamp is gone
    $versionH = Join-Path $repo 'host\version.h'
    (Get-Item $versionH).LastWriteTime = Get-Date
    try {
        & cmd /c "`"$repo\host\build.cmd`""
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
    } finally { Remove-Item -Force $stamp -ErrorAction SilentlyContinue; (Get-Item $versionH).LastWriteTime = Get-Date }
    if (-not (Select-String -Path "$build\asura.exe" -Pattern $Version -SimpleMatch -Quiet)) { throw "asura.exe does not carry version $Version" }
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

$built = Get-Date -Format 'yyyy-MM-dd'
@"
Asura's Wrath - PC recompilation
Version $Version (commit $commit, $built) - beta test build, not for redistribution
==========================================================================

You need your own copy of the Xbox 360 game. Nothing from the game is included here.
Beta testers: please read BETA_TESTING.txt too.

Requirements
  - Windows 10 or 11, 64-bit. ("N" editions of Windows need the Media Feature Pack.)
  - A graphics card with Direct3D 11 (feature level 11.0 or newer) and a current driver.
  - An Xbox-compatible controller (XInput). Keyboard and mouse are not supported yet.
    PlayStation and other controllers work through Steam Input or a similar tool.
  - About 11 GB of free space with all DLC (6.2 GB without).
  - Put this folder somewhere you can write to, e.g. C:\Games\AsurasWrath. Not in Program Files.
    Its full path must be at most 140 characters, and plain English letters are safest.

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
   with a progress screen. Later starts go straight in (a new version compiles once more).

Run asura_setup.exe again at any time to add DLC.

Playing
  - Options (resolution, frame rate, Story Mode, vsync, button labels...): press Back/View on the pad
    or F1 on the title screen.
  - Alt+Enter or F11 switches between a window and full screen.
  - To quit, close the window (or Alt+F4); there is no Exit option in the game's menus.

If something goes wrong
  - If the game crashes or stops with an error, it saves a report in the logs folder and says so.
  - To record a log, start asura_debug.cmd instead of asura.exe. The log is saved in the logs folder.
  - Please send the files from the logs folder with a short description of what happened.

Updating to a new build: copy the new files over these. Your game data (game\, private\), saves and
settings stay. Do not copy the game\ or private\ folders to anyone: they hold the game itself.
"@ | Set-Content -Encoding utf8 "$Out\README.txt"

@"
Asura's Wrath recomp - beta testing ($Version, commit $commit)
================================================================

Thank you for testing. This build is for you only: please don't share it, and never share the game\ or
private\ folders that setup creates (they hold the game itself).

What to try
  1. Setup with your own files. Do all of them turn green? If one stays red, tell us which file, where
     you got it (your own console dump, Xenia folder...), and the SHA-256 setup shows for it.
  2. The first start: the "Compiling shaders" screen should reach 100% and the game should start.
     Roughly how long did it take?
  3. Play as far as you like. Episodes 1-3 are the best start; the DLC episodes if you have them.
  4. Options (Back/View or F1 on the title screen): Internal Resolution, Frame Rate 30/60, Vsync,
     full screen (Alt+Enter), Story Mode, Button Labels. Does each change do what it says?
  5. Saving and loading: quit and continue later. Does your progress come back?
  6. Performance: turn on Show FPS in Options. Where does it drop below 30 (or 60)?

What we'd like to hear about
  - Crashes, freezes, or the game stopping with an error (a report is saved in logs\).
  - Graphics that look wrong: missing or flickering objects, wrong colours, broken shadows, black screens.
  - Sound problems: missing, repeating, too loud or too quiet.
  - QTEs or controls that don't respond, or a place where you can't continue.
  - Anything confusing in setup or this guide.

How to report
  Start asura_debug.cmd instead of asura.exe so a log is recorded, then send:

    Version:    $Version (commit $commit)   (also in the window title)
    PC:         Windows version, CPU, graphics card, RAM, display resolution and refresh rate
    Controller: e.g. Xbox Series pad over USB / DualSense through Steam Input
    Where:      episode and roughly where in it (a timestamp from a video is ideal)
    What:       what happened, and what you expected
    Every time? Does it happen again if you replay that part?
    Files:      everything in the logs folder (and a screenshot or short video if you can)

Known issues in this build (no need to report these)
  - Keyboard and mouse are not supported; a controller is required.
  - PlayStation / Switch button prompts are not included; Xbox prompts are shown.
  - No "Exit Game" option: close the window or press Alt+F4.
  - Story Mode (automatic QTEs and invincibility) is still being finished: a few QTEs may still show or
    not be answered, and some meters behave differently. You can switch it off in Options.
  - Now and then an object or its lighting appears a moment after a camera cut.
  - Some shadow flicker in places (for example the mountain in an overhead city shot in episode 8,
    which also happens in Xenia).
  - Crowd voice lines can repeat or overlap, and music can be loud compared with voices in some scenes.
  - The first start, and the first start of each new build, takes a few minutes for the shaders.
"@ | Set-Content -Encoding utf8 "$Out\BETA_TESTING.txt"

@"
@echo off
rem Starts the game with a log (logs\asura_<date>.log), for reporting problems.
cd /d "%~dp0"
set ASURA_SHOTRING=0
start "" "%~dp0asura.exe" --log
"@ | Set-Content -Encoding ascii "$Out\asura_debug.cmd"

# the zip to hand out: everything above, inside a folder of the same name; no game data, logs or caches from a test install
$zip = Join-Path (Split-Path -Parent $Out) "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
$stage = Join-Path ([IO.Path]::GetTempPath()) "asura_release_$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force "$stage\$name" | Out-Null
Get-ChildItem $Out | Where-Object { $_.Name -notin @('game', 'private', 'cache', 'shadercache', 'saves', 'logs', 'screenshots', 'asura.ini') } |
    Copy-Item -Destination "$stage\$name" -Recurse
Compress-Archive -Path "$stage\$name" -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force $stage
$sha = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
"$sha  $name.zip" | Set-Content -Encoding ascii "$zip.sha256.txt"

$size = (Get-ChildItem $Out -Recurse -File | Where-Object { $_.FullName -notmatch '\\(game|private|cache|shadercache|saves|logs)\\' } | Measure-Object Length -Sum).Sum
"Version $Version, commit $commit"
"Folder: $Out ($([math]::Round($size / 1MB)) MB)"
"Zip:    $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
"SHA-256 $sha"
