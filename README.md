# Forlorn 1.9c for Windows and Linux
An emulated port of the arm32 unreleased iOS game Forlorn by Robtop to PC

Original iPhone game running on a PC: its ARM code runs in an emulated CPU (dynarmic) and the
iOS system calls it makes are answered by the host (OpenGL graphics, sound, files).

*This project was written with AI, partially the reason why I made this open source*

First things first, what is forlorn? Forlorn is an unreleased game by Robtop (creator of Geometry Dash) that was available through
beta testing in testflight, but was later scrapped. You can find more info about it here: https://github.com/Wyliemaster/forlorn-Docs

Why make this project in the first place?: In order to play/test Forlorn at all, you'd need an iOS device running the arm32 version of iOS 10
or below. This project cleanly emulates the IPA on PC, allowing users to play and test the game straight from their PC

## Start

- Windows 10 / 11 (64-bit):  double-click "Play Forlorn.bat"
- Linux (x86-64):            ./play_linux.sh
  - The included Linux build was made on Ubuntu 26.04 via WSL (SDL2, FFmpeg 8).
  - On other distributions, or if it does not start, build it once with
    source/compile_linux.sh (it says which packages to install).

## Controls
*though the orgininal game was pure touch, Keyboard controls were added to facilitate controling the game on Keyboard and Mouse*
- Left click       touch the screen (menus, map, all on-screen buttons); during play, a left click in
  the open middle of the screen attacks
- Right click      a plain tap on the screen: switches stance (frost / fire) once fire is unlocked
- Left  / A        move left
- Right / D        move right
- Up / W / Space   jump
- X / J / Ctrl     attack
- Esc / P / Tab    in-game menu (items, quit, restart)
- Keys can be combined (run and jump at the same time). While typing your name, the keyboard
  goes to the name box: Enter to confirm, Backspace to delete.
- Closing the window saves the game first, as a phone does when the app goes to the background.

## Files

- `sandbox/`          saves (Documents) and settings; copy it to keep your progress
- `forlorn-log.txt`   the log of the last run (useful if the game closes unexpectedly)
- `source/`           the source code of ForlornHLE (one code base for both systems):
  - `compileWindows.bat`   rebuilds ForlornHLE.exe (needs Visual Studio 2019/2022 or its free Build
    Tools with "Desktop development with C++")
  - `compileLinux.bat`     rebuilds the Linux ForlornHLE from Windows, inside WSL
  - `compile_linux.sh`     rebuilds the Linux ForlornHLE on Linux (needs CMake, a C++20 compiler and the
    SDL2, OpenGL and FFmpeg development packages; without root on Debian / Ubuntu
    it can unpack them into your home folder: compile_linux.sh --local-deps)
  - `src/platform/`        the only part that differs per system: win32/ (Windows API, XAudio2, Media
    Foundation) and sdl/ (Linux: SDL2, FFmpeg, POSIX)

## Options (environment variables, set before starting ForlornHLE)

- `HLE_GDI_PRESENT=0`   Windows: show frames with SwapBuffers instead of GDI (GDI works over Remote Desktop)
- `HLE_SANDBOX=path`    keep saves somewhere else
