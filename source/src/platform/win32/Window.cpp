#include "platform/Window.h"
#include "Log.h"
#include <windows.h>
#include <windowsx.h>
#include "platform/GL.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace platform {
namespace {
HWND g_window = nullptr;
HDC g_dc = nullptr;
HGLRC g_context = nullptr;
std::mutex g_shareLock;
std::vector<HGLRC> g_sharedPool;
// The handlers are installed by other files' static initializers; as function-local statics they exist before first
// use, whatever order the files are initialized in.
std::function<void(int, double, double, int)> &touchHandler() { static std::function<void(int, double, double, int)> f; return f; }
std::function<void(std::uint32_t, int)> &keyHandler() { static std::function<void(std::uint32_t, int)> f; return f; }
std::function<void(int, bool)> &keyStateHandler() { static std::function<void(int, bool)> f; return f; }
std::function<void()> &closeHandler() { static std::function<void()> f; return f; }
int g_mouseButton = -1; // the button holding the mouse touch (0 left, 1 right), -1 when none
std::atomic<int> g_rotation{1};

// Window client coordinates -> portrait points, undoing the display rotation.
void toPoints(LPARAM lp, double &x, double &y) {
    RECT r;
    GetClientRect(g_window, &r);
    const double u = double(GET_X_LPARAM(lp)) / double(std::max<LONG>(1, r.right));
    const double v = double(GET_Y_LPARAM(lp)) / double(std::max<LONG>(1, r.bottom));
    switch (g_rotation & 3) {
    case 0: x = u * kPointsWidth; y = v * kPointsHeight; break;
    case 1: x = v * kPointsWidth; y = (1 - u) * kPointsHeight; break;
    case 2: x = (1 - u) * kPointsWidth; y = (1 - v) * kPointsHeight; break;
    default: x = (1 - v) * kPointsWidth; y = u * kPointsHeight; break;
    }
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    double x, y;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (g_mouseButton >= 0) return 0; // one mouse touch at a time
        SetCapture(hwnd);
        g_mouseButton = msg == WM_RBUTTONDOWN;
        toPoints(lp, x, y);
        if (touchHandler()) touchHandler()(0, x, y, g_mouseButton);
        return 0;
    case WM_MOUSEMOVE:
        if (g_mouseButton >= 0) { toPoints(lp, x, y); if (touchHandler()) touchHandler()(1, x, y, g_mouseButton); }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_mouseButton == int(msg == WM_RBUTTONUP)) {
            ReleaseCapture();
            const int button = g_mouseButton;
            g_mouseButton = -1;
            toPoints(lp, x, y);
            if (touchHandler()) touchHandler()(3, x, y, button);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        // Key presses and releases (auto-repeat dropped), alongside WM_CHAR's typed characters.
        if (keyStateHandler() && !(msg == WM_KEYDOWN && (lp & (1 << 30)))) keyStateHandler()(int(wp), msg == WM_KEYDOWN);
        break;
    case WM_KILLFOCUS:
        if (keyStateHandler()) keyStateHandler()(-1, false); // releases every held key
        break;
    case WM_CHAR:
        if (keyHandler()) keyHandler()(std::uint32_t(wp), 0);
        return 0;
    case WM_CLOSE:
        hlog("window", "closed");
        if (closeHandler()) closeHandler()(); // the app saves, as when a phone sends it to the background
        ExitProcess(0);
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// The client size for the current rotation: the 640x960 screen (turned), fitted into ~85% of the desktop.
void clientFor(int rotation, int &w, int &h) {
    const bool landscape = rotation & 1;
    const int pw = landscape ? kPixelsHeight : kPixelsWidth, ph = landscape ? kPixelsWidth : kPixelsHeight;
    const double fit = std::min({1.0, GetSystemMetrics(SM_CYSCREEN) * 0.85 / ph, GetSystemMetrics(SM_CXSCREEN) * 0.9 / pw});
    w = int(pw * fit);
    h = int(ph * fit);
}
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
} // namespace

void createWindow() {
    SetProcessDPIAware();
    if (const char *r = std::getenv("HLE_ROTATION")) g_rotation = std::atoi(r) & 3;
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ForlornHLE";
    RegisterClassW(&wc);
    int w, h;
    clientFor(g_rotation, w, h);
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, kStyle, FALSE);
    g_window = CreateWindowW(L"ForlornHLE", L"Forlorn (HLE)", kStyle | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    g_dc = GetDC(g_window);
    PIXELFORMATDESCRIPTOR pfd{sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    g_context = wglCreateContext(g_dc);
    for (int i = 0; i < 8; ++i) {
        HGLRC shared = wglCreateContext(g_dc);
        if (shared && wglShareLists(g_context, shared)) g_sharedPool.push_back(shared);
    }
    wglMakeCurrent(g_dc, g_context);
    hlog("window", "frames are shown through %s", gdiPresent() ? "GDI (HLE_GDI_PRESENT=0 for SwapBuffers)" : "SwapBuffers");
    hlog("window", "%dx%d window, GL %s on %s", w, h, reinterpret_cast<const char *>(glGetString(GL_VERSION)),
         reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    using SwapInterval = BOOL(WINAPI *)(int);
    if (auto swap = reinterpret_cast<SwapInterval>(wglGetProcAddress("wglSwapIntervalEXT"))) swap(1);
}
void setRotation(int quarterTurns) {
    quarterTurns &= 3;
    if (std::getenv("HLE_ROTATION") || quarterTurns == g_rotation) return;
    g_rotation = quarterTurns;
    if (!g_window) return;
    int w, h;
    clientFor(quarterTurns, w, h);
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, kStyle, FALSE);
    SetWindowPos(g_window, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    hlog("window", "rotation %d (%dx%d)", quarterTurns, w, h);
}
int rotation() { return g_rotation; }
void makeContextCurrent() { wglMakeCurrent(g_dc, g_context); }
// Contexts for other threads share the main context's objects. wglShareLists fails once the main context is current
// on another thread, so a pool is created up front (createWindow) and handed out.
void makeSharedContextCurrent() {
    HGLRC shared = nullptr;
    {
        std::lock_guard lock(g_shareLock);
        if (!g_sharedPool.empty()) { shared = g_sharedPool.back(); g_sharedPool.pop_back(); }
    }
    if (!shared) fatal("no shared GL context left for another thread");
    wglMakeCurrent(g_dc, shared);
}
// Frames go to the window through GDI by default: over Remote Desktop / remote-control tools, or with the GPU's monitor
// unplugged, an OpenGL swap chain may never reach what is shown (the window stays white). HLE_GDI_PRESENT=0 uses the
// GPU's own SwapBuffers instead.
bool gdiPresent() {
    static const bool on = [] {
        const char *v = std::getenv("HLE_GDI_PRESENT");
        return !v || std::atoi(v) != 0;
    }();
    return on;
}
void swapBuffers() {
    if (!gdiPresent()) { SwapBuffers(g_dc); return; }
    int w, h;
    clientSize(w, h);
    if (w <= 0 || h <= 0) return;
    static std::vector<unsigned char> pixels;
    pixels.resize(std::size_t(w) * std::size_t(h) * 4);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, GL_UNSIGNED_BYTE, pixels.data());
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = h; // bottom-up, like GL rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(g_dc, 0, 0, DWORD(w), DWORD(h), 0, 0, 0, UINT(h), pixels.data(), &info, DIB_RGB_COLORS);
}
void clientSize(int &width, int &height) {
    RECT r;
    GetClientRect(g_window, &r);
    width = r.right;
    height = r.bottom;
}
void pumpEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
void setTouchHandler(std::function<void(int, double, double, int)> handler) { touchHandler() = std::move(handler); }
void setKeyHandler(std::function<void(std::uint32_t, int)> handler) { keyHandler() = std::move(handler); }
bool keyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }
void setCloseHandler(std::function<void()> handler) { closeHandler() = std::move(handler); }
void setKeyStateHandler(std::function<void(int, bool)> handler) { keyStateHandler() = std::move(handler); }
void injectKey(std::uint32_t character) { if (keyHandler()) keyHandler()(character, 0); }
} // namespace platform
