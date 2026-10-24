#pragma once
// OpenGL ES 1.1 (plus the iOS OES/APPLE/IMG extensions) on desktop OpenGL (compatibility profile). The fixed-function
// pipeline maps one to one; client-side arrays and pixel pointers are guest addresses, host-addressable through the
// guest memory mapping; PVRTC textures are decoded in software.
#include <functional>
#include <string>
#include <vector>
namespace gles {
void init();                     // load entry points (main context current)
void ensureContext();            // a GL context on the calling thread (shared with the main one)
// EAGLContext support: -renderbufferStorage:fromDrawable: allocates the bound renderbuffer as the screen;
// -presentRenderbuffer: shows it in the window (turned by the window's rotation).
bool storageFromDrawable(int width, int height);
bool present();
// Saves the next presented frame (as shown in the window) as a BMP.
void requestScreenshot(const std::string &path);
// Drawn over each presented frame (window pixels): native UIKit views shown above the game, such as text fields.
void setOverlay(std::function<void(int windowWidth, int windowHeight)> draw);
// Writes 32-bit BGRA rows (bottom row first) as a BMP.
void writeBmpFile(const std::string &path, int w, int h, const std::vector<unsigned char> &bgra);
} // namespace gles
