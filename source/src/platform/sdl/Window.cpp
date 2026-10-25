// platform/Window.h on Linux: an SDL2 window with a desktop OpenGL (compatibility) context, and SDL input.
#include "platform/Window.h"
#include "platform/GL.h"
#include "Log.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace platform {
namespace {
SDL_Window *g_window = nullptr;
SDL_GLContext g_context = nullptr;
std::mutex g_shareLock;
std::vector<SDL_GLContext> g_sharedPool;
bool g_fullscreen = false;
// The handlers are installed by other files' static initializers; as function-local statics they exist before first
// use, whatever order the files are initialized in.
std::function<void(int, double, double, int)> &touchHandler() { static std::function<void(int, double, double, int)> f; return f; }
std::function<void(std::uint32_t, int)> &keyHandler() { static std::function<void(std::uint32_t, int)> f; return f; }
std::function<void(int, bool)> &keyStateHandler() { static std::function<void(int, bool)> f; return f; }
std::function<void()> &closeHandler() { static std::function<void()> f; return f; }
int g_mouseButton = -1; // the button holding the mouse touch (0 left, 1 right), -1 when none
std::atomic<int> g_rotation{1};
void toggleFullscreen() {
    const Uint32 flags = g_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(g_window, flags) != 0) fatal("SDL could not toggle fullscreen: %s", SDL_GetError());
    g_fullscreen = !g_fullscreen;
}

// Window coordinates -> portrait points, undoing the display rotation.
void toPoints(int wx, int wy, double &x, double &y) {
    int w = 1, h = 1;
    SDL_GetWindowSize(g_window, &w, &h);
    const double u = double(wx) / double(std::max(1, w)), v = double(wy) / double(std::max(1, h));
    switch (g_rotation & 3) {
    case 0: x = u * kPointsWidth; y = v * kPointsHeight; break;
    case 1: x = v * kPointsWidth; y = (1 - u) * kPointsHeight; break;
    case 2: x = (1 - u) * kPointsWidth; y = (1 - v) * kPointsHeight; break;
    default: x = (1 - v) * kPointsWidth; y = u * kPointsHeight; break;
    }
}
// The window size for the current rotation: the 640x960 screen (turned), fitted into ~85% of the desktop.
void clientFor(int rotation, int &w, int &h) {
    const bool landscape = rotation & 1;
    const int pw = landscape ? kPixelsHeight : kPixelsWidth, ph = landscape ? kPixelsWidth : kPixelsHeight;
    SDL_Rect usable{0, 0, pw, ph};
    SDL_GetDisplayUsableBounds(0, &usable);
    const double fit = std::min({1.0, usable.h * 0.85 / ph, usable.w * 0.9 / pw});
    w = int(pw * fit);
    h = int(ph * fit);
}
// SDL keys -> Window.h key codes (0: not used).
int keyCode(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + int(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + int(k - SDLK_0);
    switch (k) {
    case SDLK_LEFT: return kKeyLeft;
    case SDLK_RIGHT: return kKeyRight;
    case SDLK_UP: return kKeyUp;
    case SDLK_DOWN: return kKeyDown;
    case SDLK_SPACE: return kKeySpace;
    case SDLK_TAB: return kKeyTab;
    case SDLK_ESCAPE: return kKeyEscape;
    case SDLK_LCTRL: case SDLK_RCTRL: return kKeyControl;
    default: return 0;
    }
}
// UTF-8 text from SDL_TEXTINPUT -> code points.
void typeText(const char *text) {
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    while (*p) {
        std::uint32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3Fu >> extra;
        for (; extra && (*p & 0xC0) == 0x80; --extra) c = (c << 6) | (*p++ & 0x3F);
        if (keyHandler()) keyHandler()(c, 0);
    }
}
} // namespace

void createWindow() {
    if (const char *r = std::getenv("HLE_ROTATION")) g_rotation = std::atoi(r) & 3;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) fatal("SDL could not start its video subsystem: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    int w, h;
    clientFor(g_rotation, w, h);
    g_window = SDL_CreateWindow("Forlorn (HLE)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g_window) fatal("SDL could not create the window: %s", SDL_GetError());
    g_context = SDL_GL_CreateContext(g_window);
    if (!g_context) fatal("SDL could not create an OpenGL context: %s", SDL_GetError());
    // Contexts for other threads, sharing the main one's objects (each is created current, sharing with the current one).
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    for (int i = 0; i < 8; ++i) {
        SDL_GL_MakeCurrent(g_window, g_context);
        if (SDL_GLContext shared = SDL_GL_CreateContext(g_window)) g_sharedPool.push_back(shared);
    }
    SDL_GL_MakeCurrent(g_window, g_context);
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput();
    hlog("window", "%dx%d window (%s), GL %s on %s", w, h, SDL_GetCurrentVideoDriver(), reinterpret_cast<const char *>(glGetString(GL_VERSION)),
         reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
}
void setRotation(int quarterTurns) {
    quarterTurns &= 3;
    if (std::getenv("HLE_ROTATION") || quarterTurns == g_rotation) return;
    g_rotation = quarterTurns;
    if (!g_window || g_fullscreen) return;
    int w, h;
    clientFor(quarterTurns, w, h);
    SDL_SetWindowSize(g_window, w, h);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    hlog("window", "rotation %d (%dx%d)", quarterTurns, w, h);
}
int rotation() { return g_rotation; }
void makeContextCurrent() { SDL_GL_MakeCurrent(g_window, g_context); }
void makeSharedContextCurrent() {
    SDL_GLContext shared = nullptr;
    {
        std::lock_guard lock(g_shareLock);
        if (!g_sharedPool.empty()) { shared = g_sharedPool.back(); g_sharedPool.pop_back(); }
    }
    if (!shared) fatal("no shared GL context left for another thread");
    SDL_GL_MakeCurrent(g_window, shared);
}
bool gdiPresent() { return false; }
void swapBuffers() { SDL_GL_SwapWindow(g_window); }
void clientSize(int &width, int &height) { SDL_GL_GetDrawableSize(g_window, &width, &height); }
void pumpEvents() {
    SDL_Event e;
    double x, y;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_MOUSEBUTTONDOWN:
            if (g_mouseButton >= 0 || (e.button.button != SDL_BUTTON_LEFT && e.button.button != SDL_BUTTON_RIGHT)) break;
            g_mouseButton = e.button.button == SDL_BUTTON_RIGHT;
            SDL_CaptureMouse(SDL_TRUE);
            toPoints(e.button.x, e.button.y, x, y);
            if (touchHandler()) touchHandler()(0, x, y, g_mouseButton);
            break;
        case SDL_MOUSEMOTION:
            if (g_mouseButton >= 0) { toPoints(e.motion.x, e.motion.y, x, y); if (touchHandler()) touchHandler()(1, x, y, g_mouseButton); }
            break;
        case SDL_MOUSEBUTTONUP:
            if (g_mouseButton == int(e.button.button == SDL_BUTTON_RIGHT) && (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
                SDL_CaptureMouse(SDL_FALSE);
                const int button = g_mouseButton;
                g_mouseButton = -1;
                toPoints(e.button.x, e.button.y, x, y);
                if (touchHandler()) touchHandler()(3, x, y, button);
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool down = e.type == SDL_KEYDOWN;
            if (e.key.keysym.sym == SDLK_F11) {
                if (down && !e.key.repeat) toggleFullscreen();
                break;
            }
            if (const int key = keyCode(e.key.keysym.sym); key && !e.key.repeat && keyStateHandler()) keyStateHandler()(key, down);
            // Characters the text input events leave out (as WM_CHAR delivers them on Windows).
            if (down && keyHandler()) {
                if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) keyHandler()(13, 0);
                else if (e.key.keysym.sym == SDLK_BACKSPACE) keyHandler()(8, 0);
                else if (e.key.keysym.sym == SDLK_ESCAPE) keyHandler()(27, 0);
            }
            break;
        }
        case SDL_TEXTINPUT:
            typeText(e.text.text);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && keyStateHandler()) keyStateHandler()(-1, false);
            break;
        case SDL_QUIT:
            hlog("window", "closed");
            if (closeHandler()) closeHandler()(); // the app saves, as when a phone sends it to the background
            std::_Exit(0);
        }
    }
}
bool keyDown(int key) {
    const Uint8 *state = SDL_GetKeyboardState(nullptr);
    auto down = [&](SDL_Keycode k) { return state[SDL_GetScancodeFromKey(k)] != 0; };
    if (key >= 'A' && key <= 'Z') return down(SDLK_a + (key - 'A'));
    if (key >= '0' && key <= '9') return down(SDLK_0 + (key - '0'));
    switch (key) {
    case kKeyLeft: return down(SDLK_LEFT);
    case kKeyRight: return down(SDLK_RIGHT);
    case kKeyUp: return down(SDLK_UP);
    case kKeyDown: return down(SDLK_DOWN);
    case kKeySpace: return down(SDLK_SPACE);
    case kKeyTab: return down(SDLK_TAB);
    case kKeyEscape: return down(SDLK_ESCAPE);
    case kKeyControl: return down(SDLK_LCTRL) || down(SDLK_RCTRL);
    default: return false;
    }
}
void setTouchHandler(std::function<void(int, double, double, int)> handler) { touchHandler() = std::move(handler); }
void setKeyHandler(std::function<void(std::uint32_t, int)> handler) { keyHandler() = std::move(handler); }
void setCloseHandler(std::function<void()> handler) { closeHandler() = std::move(handler); }
void setKeyStateHandler(std::function<void(int, bool)> handler) { keyStateHandler() = std::move(handler); }
void injectKey(std::uint32_t character) { if (keyHandler()) keyHandler()(character, 0); }
} // namespace platform
