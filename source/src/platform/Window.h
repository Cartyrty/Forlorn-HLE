#pragma once
// The emulated device screen: an iPhone 4 (iPhone3,1), 320x480 points at scale 2 (640x960 pixels, portrait), shown in
// a Win32 window with a desktop OpenGL (compatibility profile) context for the app's OpenGL ES 1.1 calls. Games that
// run in landscape draw rotated into the portrait screen (cocos2d's device orientation); the window shows the screen
// turned by setRotation() quarter turns and maps the mouse back into portrait points.
#include <cstdint>
#include <functional>

namespace platform {
constexpr int kPointsWidth = 320, kPointsHeight = 480;
constexpr double kScale = 2.0;
constexpr int kPixelsWidth = 640, kPixelsHeight = 960;

void createWindow();
void makeContextCurrent();       // the main GL context on the calling thread
void makeSharedContextCurrent(); // a context sharing objects with the main one, for another thread
void swapBuffers();
bool gdiPresent(); // frames shown through GDI (Remote Desktop)
void clientSize(int &width, int &height);
void pumpEvents();
// Quarter turns (clockwise) from the portrait screen to the window: 0 portrait, 1 or 3 landscape. Resizes the window.
void setRotation(int quarterTurns);
int rotation();
// Touch input in portrait points (mouse: one finger). phase: 0 began, 1 moved, 3 ended, 4 cancelled; button: 0 left,
// 1 right.
void setTouchHandler(std::function<void(int phase, double x, double y, int button)> handler);
void setKeyHandler(std::function<void(std::uint32_t character, int key)> handler);
// Runs (on the window's thread, the app's main thread) when the window is closed, before the process exits.
void setCloseHandler(std::function<void()> handler);
// Keys: letters and digits are their upper-case ASCII codes; the others below (Windows virtual-key values).
constexpr int kKeyTab = 0x09, kKeyEscape = 0x1B, kKeySpace = 0x20, kKeyControl = 0x11, kKeyLeft = 0x25, kKeyUp = 0x26,
              kKeyRight = 0x27, kKeyDown = 0x28;
bool keyDown(int key);
// Key presses and releases (no auto-repeat); key -1 with down=false when the window loses focus.
void setKeyStateHandler(std::function<void(int virtualKey, bool down)> handler);
// Feeds a typed character as if from the keyboard (scripted tests).
void injectKey(std::uint32_t character);
} // namespace platform
