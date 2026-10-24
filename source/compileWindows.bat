@echo off
rem Compiles the Windows ForlornHLE.exe from this folder and puts it next to "Play Forlorn.bat" (one folder up).
rem Needs Visual Studio 2019 or 2022 (or its Build Tools) with "Desktop development with C++"; CMake comes with it.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto novs

set "CMAKE=cmake"
where cmake >nul 2>nul || set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not "%CMAKE%"=="cmake" if not exist "%CMAKE%" (
    echo CMake was not found. Install it from https://cmake.org or add "C++ CMake tools for Windows" in the Visual Studio Installer.
    goto fail
)

echo Configuring...
"%CMAKE%" -S . -B build -A x64 || goto fail
echo Compiling (this takes a few minutes the first time)...
"%CMAKE%" --build build --config Release --parallel || goto fail
copy /y "build\Release\ForlornHLE.exe" "..\ForlornHLE.exe" >nul || goto fail
echo.
echo Done: ForlornHLE.exe was rebuilt. Start the game with "Play Forlorn.bat".
pause
exit /b 0

:novs
echo Visual Studio with the C++ tools was not found.
echo Install "Build Tools for Visual Studio" from https://visualstudio.microsoft.com/downloads/
echo and select the "Desktop development with C++" workload, then run this again.
:fail
echo.
echo Compiling failed (see the messages above).
echo If they mention paths that cannot be found or created, the folder is too deep for Windows' path limit:
echo move the Forlorn folder somewhere short (for example C:\Games\Forlorn) and run this again.
pause
exit /b 1
