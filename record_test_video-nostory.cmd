@echo off
rem Record a play-through of any episode as video too: every frame goes to logs\rec_<date>_<time>.mp4 (H.264, Windows' own encoder,
rem about 1 MB a second at 1280x720; a new file every 5 minutes: _2.mp4, _3.mp4 ...). Each video has a .frames.txt beside it that maps
rem video frames to game frames, so a time in the video (minutes:seconds) leads to the log and the replay.
rem Otherwise the same as record_test.cmd:
rem   - Pad input to replays\rec_<date>_<time>.txt, log to logs\rec_<date>_<time>.log.
rem   - Completed save, logos skipped: pick the episode from the Episode Menu.
rem   - Story Mode on (invincibility, automatic QTEs; BURST manual). Health and timed QTEs are logged.
rem   - F9 still saves a capture of the last few seconds with each draw, for a closer look at one moment.
rem   - 30 fps. Back/Select skips cutscenes.
rem Close the game with the window's close button (or quit from the game) so the last video file is finished properly.
setlocal
cd /d "%~dp0"
for /f "delims==" %%v in ('set ASURA_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%t
set ASURA_SAVESEED=saves_complete\SAVEDATA.DAT
set ASURA_SKIPINTRO=1
set ASURA_FPS=30
set ASURA_INVINCIBLE=1
rem set ASURA_QTESKIP=1
set ASURA_HEALTHLOG=1
set ASURA_SYNCLOG=1
set ASURA_INPUTREC=replays\rec_%STAMP%.txt
set ASURA_LOGFILE=logs\rec_%STAMP%.log
set ASURA_VIDEO=logs\rec_%STAMP%.mp4
echo Recording to replays\rec_%STAMP%.txt, video logs\rec_%STAMP%.mp4, log logs\rec_%STAMP%.log
start "" "%~dp0host\build\asura.exe" --log
