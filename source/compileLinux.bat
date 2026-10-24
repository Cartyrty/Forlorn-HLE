@echo off
rem Compiles the Linux ForlornHLE from Windows, inside WSL (Windows Subsystem for Linux), and puts it next to
rem play_linux.sh (one folder up). On a Linux PC, run compile_linux.sh instead.
setlocal
cd /d "%~dp0"
where wsl >nul 2>nul || goto nowsl
set "SCRIPT="
for /f "usebackq delims=" %%p in (`wsl wslpath -a "%~dp0compile_linux.sh"`) do set "SCRIPT=%%p"
if not defined SCRIPT goto nowsl
wsl -e bash "%SCRIPT%" || goto fail
echo.
echo The Linux build is done. To play it in WSL, run in this folder's parent:  wsl ./play_linux.sh
pause
exit /b 0

:nowsl
echo WSL (Windows Subsystem for Linux) with a Linux distribution is needed: run "wsl --install" in an
echo administrator command prompt, restart, then run this again.
:fail
echo.
echo Compiling for Linux failed (see the messages above).
pause
exit /b 1
