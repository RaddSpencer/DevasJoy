@echo off
rem Story Mode run that records how the episode 22 boss's own QTE prompts (the red rings with Devanagari letters) are made.
rem   - Same as record_test.cmd (completed save, logos skipped, 30 fps, Story Mode on: invincibility and automatic QTEs, BURST manual).
rem   - Each QTE Story Mode passes is logged with all its properties ([qteskip], [props] lines), so a boss prompt that is a QTE shows
rem     up there with its button and icon type.
rem   - Every texture is dumped once to mods\dump (all sizes), so the boss prompt's letter textures get their hashes.
rem   - Press F8 while a boss prompt is on screen: it writes a [mark] line with the frame. F9 saves a capture of the last few seconds.
setlocal
cd /d "%~dp0"
for /f "delims==" %%v in ('set ASURA_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%t
set ASURA_SAVESEED=saves_complete\SAVEDATA.DAT
set ASURA_SKIPINTRO=1
set ASURA_FPS=30
set ASURA_INVINCIBLE=1
set ASURA_QTESKIP=1
set ASURA_QTEPROPS=1
set ASURA_MODDUMP=1
set ASURA_INPUTREC=replays\rec_%STAMP%.txt
set ASURA_LOGFILE=logs\rec_%STAMP%.log
echo Recording to replays\rec_%STAMP%.txt, log logs\rec_%STAMP%.log; press F8 when a boss prompt shows
start "" "%~dp0host\build\asura.exe" --log
