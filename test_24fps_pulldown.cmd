@echo off
rem Test launch: 24 fps with 2:3 pulldown, as DVD players show film on a 60 Hz TV (vsync, no tearing, slight film judder).
rem Set the display to 60 Hz (72 Hz gives an even 3:3); over about 100 Hz it falls back to one refresh per frame.
rem The FPS counter is shown in the top-left corner. Settings here override asura.ini for this run only.
setlocal
cd /d "%~dp0"
for /f "delims==" %%v in ('set ASURA_ 2^>nul') do set "%%v="
set ASURA_FPS=24
set ASURA_PULLDOWN=1
set ASURA_SHOWFPS=1
start "" "%~dp0host\build\asura.exe"
