@echo off
rem Play an episode and collect its button-prompt textures for the PlayStation / Switch labels (tools\make_prompts.ps1).
rem   - Every new texture of the sizes button prompts use is saved once to mods\dump\ (game art: stays out of git).
rem   - QTE prompts are shown (not skipped) so their icons get drawn, and answered automatically (ASURA_AUTOQTE); invincibility on.
rem   - Pad input to replays\rec_<date>_<time>.txt, log to logs\rec_<date>_<time>.log. F8 marks a moment in the log.
rem   - Completed save, logos skipped: pick the episode from the Episode Menu. Close with the window's close button.
setlocal
cd /d "%~dp0"
for /f "delims==" %%v in ('set ASURA_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%t
set ASURA_SAVESEED=saves\SAVEDATA.DAT
set ASURA_SKIPINTRO=1
set ASURA_FPS=30
set ASURA_INVINCIBLE=1
set ASURA_QTESKIP=0
set ASURA_AUTOQTE=0
set ASURA_INPUTREC=replays\rec_%STAMP%.txt
set ASURA_LOGFILE=logs\rec_%STAMP%.log
set ASURA_MODDUMP=128x128,64x64,128x64,64x32,256x128
echo Recording to replays\rec_%STAMP%.txt and logs\rec_%STAMP%.log; new prompt textures go to mods\dump
start "" "%~dp0host\build\asura.exe" --log
