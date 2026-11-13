@echo off
rem Test launch: 24 fps for FreeSync / G-Sync displays (presents as soon as each frame is ready, tearing allowed).
rem Without a variable refresh rate display the picture will tear. The FPS counter is shown in the top-left corner.
rem Settings here override asura.ini for this run only; the Options menu (Back / F1 on the title screen) shows them.
setlocal
cd /d "%~dp0"
for /f "delims==" %%v in ('set ASURA_ 2^>nul') do set "%%v="
set ASURA_FPS=24
set ASURA_VRR=1
set ASURA_SHOWFPS=1
start "" "%~dp0host\build\asura.exe"
