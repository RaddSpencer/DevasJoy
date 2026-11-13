@echo off
rem Record a play-through of any episode for testing. Pad input goes to replays\rec_<date>_<time>.txt, the log to logs\rec_<date>_<time>.log.
rem   - Starts from the completed save (saves_complete\SAVEDATA.DAT, copied over the installed save as every test run does), logos skipped:
rem     pick the episode from the Episode Menu.
rem   - Story Mode on: invincibility and automatic QTEs (BURST stays manual). In a fight with a life meter, take hits: the meter should
rem     stay full. The player's Health is logged ([health]).
rem   - Timed, graded (Synchronic) QTEs are logged ([sync], with the track's properties).
rem   - F9 saves a capture of the last few seconds (logs\), for rendering problems.
rem   - 30 fps, so the recording plays back the way test runs are set up. Back/Select skips cutscenes.
setlocal
cd /d "%~dp0"
for /f "delims==" %%v in ('set ASURA_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%t
set ASURA_SAVESEED=saves_complete\SAVEDATA.DAT
set ASURA_SKIPINTRO=1
set ASURA_FPS=30
set ASURA_INVINCIBLE=1
set ASURA_QTESKIP=1
set ASURA_HEALTHLOG=1
set ASURA_SYNCLOG=1
set ASURA_INPUTREC=replays\rec_%STAMP%.txt
set ASURA_LOGFILE=logs\rec_%STAMP%.log
echo Recording to replays\rec_%STAMP%.txt, log logs\rec_%STAMP%.log
start "" "%~dp0host\build\asura.exe" --log
