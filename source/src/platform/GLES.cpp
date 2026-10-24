#include "platform/GLES.h"
#include "platform/Pvrtc.h"
#include "platform/Window.h"
#include "Hle.h"
#include "Log.h"
#include "Memory.h"
#include "libc/System.h"
#include "platform/GL.h"
#include "platform/Platform.h"
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using hle::arg;
using hle::argFloat;
using hle::ret;

namespace {
// Desktop GL constants beyond GL 1.1 (the OES/APPLE values are the same numbers).
constexpr GLenum FRAMEBUFFER = 0x8D40, RENDERBUFFER = 0x8D41, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum FRAMEBUFFER_BINDING = 0x8CA6, READ_FRAMEBUFFER_BINDING = 0x8CAA, RENDERBUFFER_BINDING = 0x8CA7;
constexpr GLenum COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, ARRAY_BUFFER_BINDING = 0x8894, ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
constexpr GLenum TEXTURE0 = 0x84C0, ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum RGBA8 = 0x8058, BGRA = 0x80E1, RGB565_OES = 0x8D62;
constexpr GLenum POINT_SIZE_ARRAY_OES = 0x8B9C;
constexpr GLenum kFixed = 0x140C;

void *proc(const char *name) {
    return platform::glProc(name);
}
template <class F> F fn(const char *name) {
    static std::unordered_map<std::string, void *> cache;
    static std::mutex lock;
    std::lock_guard guard(lock);
    auto &slot = cache[name];
    if (!slot) slot = proc(name);
    if (!slot) fatal("missing OpenGL entry point %s", name);
    return reinterpret_cast<F>(slot);
}
#define GLF(ret_, name, ...) fn<ret_(APIENTRY *)(__VA_ARGS__)>(#name)
const void *P(std::uint64_t a) { return a ? mem::base() + a : nullptr; }
void *PW(std::uint64_t a) { return a ? mem::base() + a : nullptr; }
thread_local bool t_hasContext = false;
std::uint32_t g_drawableRenderbuffer = 0, g_presentFbo = 0, g_blitFbo = 0, g_presentTexture = 0;
int g_drawableWidth = 0, g_drawableHeight = 0, g_textureWidth = 0, g_textureHeight = 0;
GLint binding(GLenum which) {
    GLint v = 0;
    glGetIntegerv(which, &v);
    return v;
}
// A pointer argument for an array or index source: an offset while a buffer object is bound, else guest memory.
const void *arrayPointer(GLenum bindingName, std::uint64_t pointer) {
    if (binding(bindingName)) return reinterpret_cast<const void *>(std::uintptr_t(pointer));
    return P(pointer);
}
void warnOnce(const char *what) {
    static std::mutex lock;
    static std::vector<std::string> seen;
    std::lock_guard guard(lock);
    for (auto &s : seen) if (s == what) return;
    seen.push_back(what);
    hlog("gl", "%s", what);
}
// Point sizes per vertex (glPointSizePointerOES, cocos2d's point particle systems): kept guest-side and applied by
// drawing the points one by one.
struct PointSizes { GLenum type = GL_FLOAT; GLsizei stride = 0; std::uint64_t pointer = 0; bool enabled = false; };
thread_local PointSizes t_pointSizes;
// Guest copies of the glGetString strings.
std::uint64_t guestString(const char *text) {
    static std::mutex lock;
    static std::unordered_map<std::string, std::uint64_t> strings;
    std::lock_guard guard(lock);
    auto &slot = strings[text];
    if (!slot) slot = mem::hleString(text);
    return slot;
}
} // namespace

namespace gles {
void init() {
    t_hasContext = true;
    hlog("gl", "GL_VERSION %s", reinterpret_cast<const char *>(glGetString(GL_VERSION)));
}
void ensureContext() {
    if (t_hasContext) return;
    platform::makeSharedContextCurrent();
    t_hasContext = true;
    hlog("gl", "shared GL context created for a background thread");
}
bool storageFromDrawable(int width, int height) {
    ensureContext();
    g_drawableRenderbuffer = std::uint32_t(binding(RENDERBUFFER_BINDING));
    g_drawableWidth = width;
    g_drawableHeight = height;
    GLF(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)(RENDERBUFFER, RGBA8, width, height);
    hlog("gl", "drawable renderbuffer %u is %dx%d", g_drawableRenderbuffer, width, height);
    return true;
}
std::string g_pendingShot;
// Installed by another file's static initializer: a function-local static exists before first use.
std::function<void(int, int)> &overlay() { static std::function<void(int, int)> f; return f; }
void setOverlay(std::function<void(int, int)> draw) { overlay() = std::move(draw); }
void requestScreenshot(const std::string &path) { g_pendingShot = path; }
void writeBmpFile(const std::string &path, int w, int h, const std::vector<unsigned char> &bgra) {
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        const std::uint32_t size = std::uint32_t(54 + bgra.size());
        unsigned char header[54] = {'B', 'M'};
        std::memcpy(header + 2, &size, 4);
        header[10] = 54;
        header[14] = 40;
        std::memcpy(header + 18, &w, 4);
        std::memcpy(header + 22, &h, 4);
        header[26] = 1;
        header[28] = 32;
        std::fwrite(header, 1, 54, f);
        std::fwrite(bgra.data(), 1, bgra.size(), f);
        std::fclose(f);
    }
}
// Copies the drawable into a texture and draws it turned onto the window, leaving the app's GL state untouched.
static void checkpoint(const char *where) {
    static const bool debug = std::getenv("HLE_GLDEBUG") != nullptr;
    if (!debug) return;
    if (const auto e = glGetError()) hlog("gl", "present: error 0x%x at %s", e, where);
}
bool present() {
    if (!g_drawableRenderbuffer) return false;
    checkpoint("entry");
    const GLint read = binding(READ_FRAMEBUFFER_BINDING), draw = binding(FRAMEBUFFER_BINDING);
    const GLint activeTexture = binding(ACTIVE_TEXTURE), arrayBuffer = binding(ARRAY_BUFFER_BINDING);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    GLF(void, glActiveTexture, GLenum)(TEXTURE0);
    const GLint texture = binding(GL_TEXTURE_BINDING_2D);
    if (!g_presentFbo) GLF(void, glGenFramebuffers, GLsizei, GLuint *)(1, &g_presentFbo);
    if (!g_blitFbo) GLF(void, glGenFramebuffers, GLsizei, GLuint *)(1, &g_blitFbo);
    if (!g_presentTexture || g_textureWidth != g_drawableWidth || g_textureHeight != g_drawableHeight) {
        if (!g_presentTexture) glGenTextures(1, &g_presentTexture);
        glBindTexture(GL_TEXTURE_2D, g_presentTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_drawableWidth, g_drawableHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
        g_textureWidth = g_drawableWidth;
        g_textureHeight = g_drawableHeight;
        GLF(void, glBindFramebuffer, GLenum, GLuint)(FRAMEBUFFER, g_blitFbo);
        GLF(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)(FRAMEBUFFER, COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_presentTexture, 0);
    }
    auto bindFb = GLF(void, glBindFramebuffer, GLenum, GLuint);
    bindFb(READ_FRAMEBUFFER, g_presentFbo);
    GLF(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, RENDERBUFFER, g_drawableRenderbuffer);
    bindFb(DRAW_FRAMEBUFFER, g_blitFbo);
    glDisable(GL_SCISSOR_TEST);
    GLF(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
    (0, 0, g_drawableWidth, g_drawableHeight, 0, 0, g_drawableWidth, g_drawableHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    bindFb(FRAMEBUFFER, 0);
    int w, h;
    platform::clientSize(w, h);
    glViewport(0, 0, w, h);
    for (GLenum cap : {GL_DEPTH_TEST, GL_BLEND, GL_ALPHA_TEST, GL_CULL_FACE, GL_LIGHTING, GL_FOG, GL_STENCIL_TEST})
        glDisable(cap);
    GLF(void, glBindBuffer, GLenum, GLuint)(ARRAY_BUFFER, 0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_presentTexture);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    for (GLenum mode : {GL_PROJECTION, GL_MODELVIEW, GL_TEXTURE}) {
        glMatrixMode(mode);
        glPushMatrix();
        glLoadIdentity();
    }
    // Window corner (u, v; v down) -> portrait screen point (x, y; y down) -> texture (x, 1 - y).
    const int rotation = platform::rotation();
    auto corner = [&](float u, float v) {
        float x, y;
        switch (rotation & 3) {
        case 0: x = u; y = v; break;
        case 1: x = v; y = 1 - u; break;
        case 2: x = 1 - u; y = 1 - v; break;
        default: x = 1 - v; y = u; break;
        }
        glTexCoord2f(x, 1 - y);
        glVertex2f(u * 2 - 1, 1 - v * 2);
    };
    glBegin(GL_QUADS);
    corner(0, 1);
    corner(1, 1);
    corner(1, 0);
    corner(0, 0);
    glEnd();
    for (GLenum mode : {GL_TEXTURE, GL_MODELVIEW, GL_PROJECTION}) {
        glMatrixMode(mode);
        glPopMatrix();
    }
    glDisable(GL_TEXTURE_2D);
    if (overlay()) overlay()(w, h);
    if (!g_pendingShot.empty()) {
        std::vector<unsigned char> pixels(std::size_t(w) * std::size_t(h) * 4);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, BGRA, GL_UNSIGNED_BYTE, pixels.data());
        writeBmpFile(g_pendingShot, w, h, pixels);
        g_pendingShot.clear();
    }
    platform::swapBuffers();
    // The app's framebuffers go back first: glPopAttrib restores its draw buffer (COLOR_ATTACHMENT0), which is only
    // valid with a framebuffer object bound.
    bindFb(READ_FRAMEBUFFER, GLuint(read));
    bindFb(DRAW_FRAMEBUFFER, GLuint(draw));
    glBindTexture(GL_TEXTURE_2D, GLuint(texture));
    glPopClientAttrib();
    glPopAttrib();
    GLF(void, glBindBuffer, GLenum, GLuint)(ARRAY_BUFFER, GLuint(arrayBuffer));
    GLF(void, glActiveTexture, GLenum)(GLenum(activeTexture));
    checkpoint("exit");
    static int frames = 0;
    if (++frames == 1 || frames % 600 == 0) hlog("gl", "presented frame %d", frames);
    return true;
}
} // namespace gles

// ======================================== OpenGL ES 1.1 ========================================
// Texture work on a loading thread (its own context sharing the main one's objects) must reach the GPU before the
// main context uses the texture: desktop GL makes it visible only after the uploading context flushes.
static void publish() { if (!sys::isMainThread()) glFinish(); }
// HLE_GLTRACE=1 logs texture uploads, parameters and bindings.
static bool trace() { static const bool on = std::getenv("HLE_GLTRACE") != nullptr; return on; }
HLE(glGetError) { ret(glGetError()); }
HLE(glGetString) {
    switch (GLenum(arg(0))) {
    case GL_VENDOR: return ret(guestString("Imagination Technologies"));
    case GL_RENDERER: return ret(guestString("PowerVR SGX 535"));
    case GL_VERSION: return ret(guestString("OpenGL ES-CM 1.1 IMGSGX535-63.14.2"));
    case GL_EXTENSIONS:
        return ret(guestString("GL_APPLE_framebuffer_multisample GL_APPLE_texture_2D_limited_npot GL_APPLE_texture_format_BGRA8888 "
                               "GL_APPLE_texture_max_level GL_EXT_blend_minmax GL_EXT_discard_framebuffer GL_EXT_read_format_bgra "
                               "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias GL_IMG_read_format "
                               "GL_IMG_texture_compression_pvrtc GL_OES_blend_equation_separate GL_OES_blend_func_separate "
                               "GL_OES_blend_subtract GL_OES_compressed_paletted_texture GL_OES_depth24 GL_OES_draw_texture "
                               "GL_OES_fbo_render_mipmap GL_OES_framebuffer_object GL_OES_mapbuffer GL_OES_matrix_palette "
                               "GL_OES_packed_depth_stencil GL_OES_point_size_array GL_OES_point_sprite GL_OES_read_format "
                               "GL_OES_rgb8_rgba8 GL_OES_stencil8 GL_OES_texture_mirrored_repeat GL_OES_vertex_array_object"));
    }
    ret(0);
}
HLE(glGetIntegerv) {
    const GLenum pname = GLenum(arg(0));
    if (pname == GL_MAX_TEXTURE_SIZE) return mem::w32(arg(1), 2048);
    if (pname == 0x86A2 /*NUM_COMPRESSED_TEXTURE_FORMATS*/) return mem::w32(arg(1), 4);
    glGetIntegerv(pname, static_cast<GLint *>(PW(arg(1))));
}
HLE(glGetFloatv) { glGetFloatv(GLenum(arg(0)), static_cast<GLfloat *>(PW(arg(1)))); }
HLE(glIsEnabled) { ret(GLenum(arg(0)) == POINT_SIZE_ARRAY_OES ? t_pointSizes.enabled : glIsEnabled(GLenum(arg(0)))); }
HLE(glEnable) { glEnable(GLenum(arg(0))); }
HLE(glDisable) { glDisable(GLenum(arg(0))); }
HLE(glEnableClientState) {
    if (GLenum(arg(0)) == POINT_SIZE_ARRAY_OES) { t_pointSizes.enabled = true; return; }
    glEnableClientState(GLenum(arg(0)));
}
HLE(glDisableClientState) {
    if (GLenum(arg(0)) == POINT_SIZE_ARRAY_OES) { t_pointSizes.enabled = false; return; }
    glDisableClientState(GLenum(arg(0)));
}
HLE(glHint) { glHint(GLenum(arg(0)), GLenum(arg(1))); }
HLE(glPixelStorei) { glPixelStorei(GLenum(arg(0)), GLint(arg(1))); }
HLE(glClear) { glClear(GLbitfield(arg(0))); }
HLE(glClearColor) { glClearColor(argFloat(0), argFloat(1), argFloat(2), argFloat(3)); }
HLE(glClearDepthf) { glClearDepth(argFloat(0)); }
HLE(glColor4f) { glColor4f(argFloat(0), argFloat(1), argFloat(2), argFloat(3)); }
HLE(glColor4ub) { glColor4ub(GLubyte(arg(0)), GLubyte(arg(1)), GLubyte(arg(2)), GLubyte(arg(3))); }
HLE(glAlphaFunc) { glAlphaFunc(GLenum(arg(0)), argFloat(1)); }
HLE(glBlendFunc) { glBlendFunc(GLenum(arg(0)), GLenum(arg(1))); }
HLE(glDepthFunc) { glDepthFunc(GLenum(arg(0))); }
HLE(glLineWidth) { glLineWidth(argFloat(0)); }
HLE(glPointSize) { glPointSize(argFloat(0)); }
HLE(glScissor) { glScissor(GLint(arg(0)), GLint(arg(1)), GLsizei(arg(2)), GLsizei(arg(3))); }
HLE(glViewport) { glViewport(GLint(arg(0)), GLint(arg(1)), GLsizei(arg(2)), GLsizei(arg(3))); }
// Matrices.
HLE(glMatrixMode) { glMatrixMode(GLenum(arg(0))); }
HLE(glLoadIdentity) { glLoadIdentity(); }
HLE(glPushMatrix) { glPushMatrix(); }
HLE(glPopMatrix) { glPopMatrix(); }
HLE(glMultMatrixf) { glMultMatrixf(static_cast<const GLfloat *>(P(arg(0)))); }
HLE(glLoadMatrixf) { glLoadMatrixf(static_cast<const GLfloat *>(P(arg(0)))); }
HLE(glOrthof) { glOrtho(argFloat(0), argFloat(1), argFloat(2), argFloat(3), argFloat(4), argFloat(5)); }
HLE(glFrustumf) { glFrustum(argFloat(0), argFloat(1), argFloat(2), argFloat(3), argFloat(4), argFloat(5)); }
HLE(glRotatef) { glRotatef(argFloat(0), argFloat(1), argFloat(2), argFloat(3)); }
HLE(glTranslatef) { glTranslatef(argFloat(0), argFloat(1), argFloat(2)); }
HLE(glScalef) { glScalef(argFloat(0), argFloat(1), argFloat(2)); }
// Client arrays.
static void checkFixed(GLenum type) { if (type == kFixed) warnOnce("GL_FIXED vertex data is not supported"); }
HLE(glVertexPointer) {
    checkFixed(GLenum(arg(1)));
    glVertexPointer(GLint(arg(0)), GLenum(arg(1)), GLsizei(arg(2)), arrayPointer(ARRAY_BUFFER_BINDING, arg(3)));
}
HLE(glTexCoordPointer) {
    checkFixed(GLenum(arg(1)));
    glTexCoordPointer(GLint(arg(0)), GLenum(arg(1)), GLsizei(arg(2)), arrayPointer(ARRAY_BUFFER_BINDING, arg(3)));
}
HLE(glColorPointer) {
    checkFixed(GLenum(arg(1)));
    glColorPointer(GLint(arg(0)), GLenum(arg(1)), GLsizei(arg(2)), arrayPointer(ARRAY_BUFFER_BINDING, arg(3)));
}
HLE(glNormalPointer) { glNormalPointer(GLenum(arg(0)), GLsizei(arg(1)), arrayPointer(ARRAY_BUFFER_BINDING, arg(2))); }
HLE(glPointSizePointerOES) {
    if (binding(ARRAY_BUFFER_BINDING)) warnOnce("glPointSizePointerOES from a buffer object is not supported");
    t_pointSizes.type = GLenum(arg(0));
    t_pointSizes.stride = GLsizei(arg(1));
    t_pointSizes.pointer = arg(2);
}
HLE(glDrawArrays) {
    const GLenum mode = GLenum(arg(0));
    const GLint first = GLint(arg(1));
    const GLsizei count = GLsizei(arg(2));
    if (mode == GL_POINTS && t_pointSizes.enabled && t_pointSizes.pointer && !binding(ARRAY_BUFFER_BINDING)) {
        const GLsizei stride = t_pointSizes.stride ? t_pointSizes.stride : 4;
        for (GLsizei i = 0; i < count; ++i) {
            glPointSize(mem::rf(t_pointSizes.pointer + std::uint64_t(first + i) * std::uint64_t(stride)));
            glDrawArrays(GL_POINTS, first + i, 1);
        }
        return;
    }
    glDrawArrays(mode, first, count);
}
HLE(glDrawElements) {
    glDrawElements(GLenum(arg(0)), GLsizei(arg(1)), GLenum(arg(2)), arrayPointer(ELEMENT_ARRAY_BUFFER_BINDING, arg(3)));
}
// Buffer objects.
HLE(glGenBuffers) { GLF(void, glGenBuffers, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
HLE(glDeleteBuffers) { GLF(void, glDeleteBuffers, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
HLE(glBindBuffer) { GLF(void, glBindBuffer, GLenum, GLuint)(GLenum(arg(0)), GLuint(arg(1))); }
HLE(glBufferData) {
    GLF(void, glBufferData, GLenum, std::ptrdiff_t, const void *, GLenum)(GLenum(arg(0)), std::ptrdiff_t(std::int32_t(arg(1))), P(arg(2)), GLenum(arg(3)));
}
HLE(glBufferSubData) {
    GLF(void, glBufferSubData, GLenum, std::ptrdiff_t, std::ptrdiff_t, const void *)(GLenum(arg(0)), std::ptrdiff_t(std::int32_t(arg(1))),
                                                                                  std::ptrdiff_t(std::int32_t(arg(2))), P(arg(3)));
}
// Textures.
HLE(glActiveTexture) { GLF(void, glActiveTexture, GLenum)(GLenum(arg(0))); }
HLE(glClientActiveTexture) { GLF(void, glClientActiveTexture, GLenum)(GLenum(arg(0))); }
HLE(glGenTextures) {
    glGenTextures(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1))));
    if (trace()) hlog("gltrace", "glGenTextures -> %u (thread %u)", mem::r32(arg(1)), platform::currentThreadId());
}
HLE(glDeleteTextures) { glDeleteTextures(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
HLE(glBindTexture) { if (trace()) hlog("gltrace", "glBindTexture %u", unsigned(arg(1))); glBindTexture(GLenum(arg(0)), GLuint(arg(1))); }
HLE(glTexParameteri) {
    if (trace()) hlog("gltrace", "glTexParameteri(0x%x, 0x%x, 0x%x) tex %d", unsigned(arg(0)), unsigned(arg(1)), unsigned(arg(2)), binding(GL_TEXTURE_BINDING_2D));
    glTexParameteri(GLenum(arg(0)), GLenum(arg(1)), GLint(arg(2)));
publish(); }
HLE(glTexParameterf) { glTexParameterf(GLenum(arg(0)), GLenum(arg(1)), argFloat(2)); }
HLE(glTexEnvi) { glTexEnvi(GLenum(arg(0)), GLenum(arg(1)), GLint(arg(2))); }
HLE(glTexEnvf) { glTexEnvf(GLenum(arg(0)), GLenum(arg(1)), argFloat(2)); }
// glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels): GLES takes the unsized
// format as the internal format (BGRA too, with APPLE_texture_format_BGRA8888).
HLE(glTexImage2D) {
    GLint internal = GLint(arg(2));
    // HLE_DUMP_TEX=<dir>: RGBA8 / A8 uploads as BMPs.
    if (const char *dir = std::getenv("HLE_DUMP_TEX"); dir && arg(1) == 0 && arg(8) && (GLenum(arg(7)) == GL_UNSIGNED_BYTE || GLenum(arg(7)) == 0x8033)) {
        static int n = 0;
        const int w = int(arg(3)), h = int(arg(4));
        const GLenum format = GLenum(arg(6));
        const int bpp = format == GL_RGBA ? 4 : format == GL_RGB ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
        std::vector<unsigned char> bgra(std::size_t(w) * h * 4);
        const auto *src = mem::base() + arg(8);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                auto *o = &bgra[(std::size_t(h - 1 - y) * w + x) * 4];
                if (GLenum(arg(7)) == 0x8033) {
                    const auto v = mem::read<std::uint16_t>(arg(8) + (std::size_t(y) * w + x) * 2);
                    o[2] = std::uint8_t((v >> 12) * 17); o[1] = std::uint8_t(((v >> 8) & 15) * 17); o[0] = std::uint8_t(((v >> 4) & 15) * 17); o[3] = std::uint8_t((v & 15) * 17);
                    continue;
                }
                const auto *p = src + (std::size_t(y) * w + x) * bpp;
                if (bpp >= 3) { o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = bpp == 4 ? p[3] : 255; }
                else { o[0] = o[1] = o[2] = bpp == 2 ? p[0] : 255; o[3] = bpp == 2 ? p[1] : p[0]; }
            }
        gles::writeBmpFile(std::string(dir) + "/tex" + std::to_string(n++) + "_" + std::to_string(w) + "x" + std::to_string(h) + ".bmp", w, h, bgra);
    }
    if (trace()) hlog("gltrace", "glTexImage2D level %u %ux%u internal 0x%x format 0x%x type 0x%x tex %d", unsigned(arg(1)), unsigned(arg(3)), unsigned(arg(4)), unsigned(arg(2)), unsigned(arg(6)), unsigned(arg(7)), binding(GL_TEXTURE_BINDING_2D));
    if (internal == GLint(BGRA)) internal = GL_RGBA;
    glTexImage2D(GLenum(arg(0)), GLint(arg(1)), internal, GLsizei(arg(3)), GLsizei(arg(4)), GLint(arg(5)), GLenum(arg(6)), GLenum(arg(7)), P(arg(8)));
publish(); }
HLE(glTexSubImage2D) {
    glTexSubImage2D(GLenum(arg(0)), GLint(arg(1)), GLint(arg(2)), GLint(arg(3)), GLsizei(arg(4)), GLsizei(arg(5)), GLenum(arg(6)), GLenum(arg(7)),
                    P(arg(8)));
publish(); }
// glCompressedTexImage2D(target, level, internalformat, width, height, border, imageSize, data): PVRTC
// (RGB/RGBA 4 bpp 0x8C00/0x8C02, 2 bpp 0x8C01/0x8C03) is decoded to RGBA8.
HLE(glCompressedTexImage2D) {
    const GLenum format = GLenum(arg(2));
    const int width = int(arg(3)), height = int(arg(4));
    if (trace()) hlog("gltrace", "glCompressedTexImage2D level %u %dx%d format 0x%x tex %d", unsigned(arg(1)), width, height, format, binding(GL_TEXTURE_BINDING_2D));
    if (format >= 0x8C00 && format <= 0x8C03) {
        const auto pixels = pvrtc::decode(mem::base() + arg(7), width, height, format == 0x8C01 || format == 0x8C03);
        // HLE_DUMP_PVRTC=<dir>: decoded textures as BMPs (debugging the decoder).
        if (const char *dir = std::getenv("HLE_DUMP_PVRTC"); dir && arg(1) == 0) {
            static int n = 0;
            std::vector<unsigned char> bgra(pixels.size());
            for (std::size_t i = 0; i < pixels.size(); i += 4) { bgra[i] = pixels[i + 2]; bgra[i + 1] = pixels[i + 1]; bgra[i + 2] = pixels[i]; bgra[i + 3] = pixels[i + 3]; }
            gles::writeBmpFile(std::string(dir) + "/pvrtc" + std::to_string(n++) + ".bmp", width, height, bgra);
        }
        glTexImage2D(GLenum(arg(0)), GLint(arg(1)), GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        publish();
        return;
    }
    hlog("gl", "unsupported compressed texture format 0x%x", format);
publish(); }
HLE(glCopyTexImage2D) {
    glCopyTexImage2D(GLenum(arg(0)), GLint(arg(1)), GLenum(arg(2)), GLint(arg(3)), GLint(arg(4)), GLsizei(arg(5)), GLsizei(arg(6)), GLint(arg(7)));
}
HLE(glReadPixels) {
    glReadPixels(GLint(arg(0)), GLint(arg(1)), GLsizei(arg(2)), GLsizei(arg(3)), GLenum(arg(4)), GLenum(arg(5)), PW(arg(6)));
}
HLE(glGenerateMipmapOES) { GLF(void, glGenerateMipmap, GLenum)(GLenum(arg(0)));  publish(); }
// Framebuffer objects (OES_framebuffer_object, APPLE_framebuffer_multisample).
HLE(glGenFramebuffersOES) { GLF(void, glGenFramebuffers, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
HLE(glDeleteFramebuffersOES) { GLF(void, glDeleteFramebuffers, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
HLE(glBindFramebufferOES) { GLF(void, glBindFramebuffer, GLenum, GLuint)(GLenum(arg(0)), GLuint(arg(1))); }
HLE(glCheckFramebufferStatusOES) { ret(GLF(GLenum, glCheckFramebufferStatus, GLenum)(GLenum(arg(0)))); }
HLE(glFramebufferRenderbufferOES) {
    GLF(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)(GLenum(arg(0)), GLenum(arg(1)), GLenum(arg(2)), GLuint(arg(3)));
}
HLE(glFramebufferTexture2DOES) {
    GLF(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)(GLenum(arg(0)), GLenum(arg(1)), GLenum(arg(2)), GLuint(arg(3)),
                                                                          GLint(arg(4)));
}
HLE(glGenRenderbuffersOES) { GLF(void, glGenRenderbuffers, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
HLE(glDeleteRenderbuffersOES) {
    GLF(void, glDeleteRenderbuffers, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1))));
}
HLE(glBindRenderbufferOES) { GLF(void, glBindRenderbuffer, GLenum, GLuint)(GLenum(arg(0)), GLuint(arg(1))); }
static GLenum renderbufferFormat(GLenum format) { return format == RGB565_OES ? RGBA8 : format; }
HLE(glRenderbufferStorageOES) {
    GLF(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)(GLenum(arg(0)), renderbufferFormat(GLenum(arg(1))), GLsizei(arg(2)),
                                                                       GLsizei(arg(3)));
}
HLE(glRenderbufferStorageMultisampleAPPLE) {
    GLF(void, glRenderbufferStorageMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei)(GLenum(arg(0)), GLsizei(arg(1)),
                                                                                       renderbufferFormat(GLenum(arg(2))), GLsizei(arg(3)),
                                                                                       GLsizei(arg(4)));
}
// glResolveMultisampleFramebufferAPPLE: the read (multisample) framebuffer into the draw framebuffer, full size.
HLE(glResolveMultisampleFramebufferAPPLE) {
    GLint w = 0, h = 0;
    GLF(void, glGetRenderbufferParameteriv, GLenum, GLenum, GLint *)(RENDERBUFFER, 0x8D42 /*WIDTH*/, &w);
    GLF(void, glGetRenderbufferParameteriv, GLenum, GLenum, GLint *)(RENDERBUFFER, 0x8D43 /*HEIGHT*/, &h);
    if (!w || !h) { w = g_drawableWidth; h = g_drawableHeight; }
    GLF(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
    (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
HLE(glGetRenderbufferParameterivOES) {
    GLF(void, glGetRenderbufferParameteriv, GLenum, GLenum, GLint *)(GLenum(arg(0)), GLenum(arg(1)), static_cast<GLint *>(PW(arg(2))));
}
HLE(glDiscardFramebufferEXT) {}
