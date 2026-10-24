// CoreGraphics bitmap contexts and images, UIImage (Apple CgBI PNGs, other PNG/JPEG through stb_image), UIFont /
// CGFont (TrueType through stb_truetype, the host's fonts standing in for the iOS ones) and UIKit string drawing.
//
// CG user space is y-up from the bottom-left of a bitmap context; the bitmap's first row is its top. The CTM maps user
// space to device space (pixels, y-up). UIKit string and image drawing works in the current context's user space
// with y down (cocos2d flips the CTM before UIGraphicsPushContext).
#include "uikit/UIKit.h"
#include "foundation/Foundation.h"
#include "libc/Files.h"
#include "Log.h"
#include "Memory.h"
#include "Runtime.h"
#include "platform/GL.h"
#include "platform/Platform.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include "stb_image.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>


using namespace ns;
using namespace uikit;
using objc::marg;
using objc::self;
using hle::arg;
using hle::argFloat;
using hle::ret;

namespace zlib {
bool decompress(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &out, int windowBits);
}
namespace {
// ---------------------------------------------------------------- affine transforms
struct Affine {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    void apply(float x, float y, float &ox, float &oy) const { ox = a * x + c * y + tx; oy = b * x + d * y + ty; }
    Affine then(const Affine &o) const { // this, then o
        return {a * o.a + b * o.c, a * o.b + b * o.d, c * o.a + d * o.c, c * o.b + d * o.d, tx * o.a + ty * o.c + o.tx, tx * o.b + ty * o.d + o.ty};
    }
    Affine inverse() const {
        const float det = a * d - b * c;
        if (det == 0) return {};
        return {d / det, -b / det, -c / det, a / det, (c * ty - d * tx) / det, (b * tx - a * ty) / det};
    }
    float scaleY() const { return std::sqrt(c * c + d * d); }
};

// ---------------------------------------------------------------- images
struct ImageData : objc::HostObject {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba; // straight (unpremultiplied) RGBA, top row first
    bool alpha = true;
};
// Apple's "CgBI" PNGs (pngcrush -iphone): a CgBI chunk first, IDAT holding raw deflate data (no zlib header), pixels
// in BGRA order with premultiplied alpha. Decoded here (8-bit RGB / RGBA, not interlaced); others go to stb_image.
bool decodeCgbi(const std::uint8_t *bytes, std::size_t size, ImageData &out) {
    if (size < 33 || bytes[0] != 0x89 || std::memcmp(bytes + 1, "PNG", 3) || std::memcmp(bytes + 12, "CgBI", 4)) return false;
    auto be32 = [&](std::size_t o) { return (std::uint32_t(bytes[o]) << 24) | (bytes[o + 1] << 16) | (bytes[o + 2] << 8) | bytes[o + 3]; };
    int width = 0, height = 0, depth = 0, colorType = 0, interlace = 0;
    std::vector<std::uint8_t> idat;
    for (std::size_t at = 8; at + 8 <= size;) {
        const auto length = be32(at);
        const std::string type(reinterpret_cast<const char *>(bytes + at + 4), 4);
        const auto *body = bytes + at + 8;
        if (at + 12 + length > size) break;
        if (type == "IHDR") {
            width = int(be32(at + 8));
            height = int(be32(at + 12));
            depth = body[8];
            colorType = body[9];
            interlace = body[12];
        } else if (type == "IDAT") idat.insert(idat.end(), body, body + length);
        else if (type == "IEND") break;
        at += 12 + length;
    }
    const int channels = colorType == 6 ? 4 : colorType == 2 ? 3 : 0;
    if (!channels || depth != 8 || interlace || width <= 0 || height <= 0) {
        hlog("image", "unsupported CgBI PNG (type %d, depth %d, interlace %d)", colorType, depth, interlace);
        return false;
    }
    std::vector<std::uint8_t> raw;
    if (!zlib::decompress(idat.data(), idat.size(), raw, -15)) return false;
    const std::size_t stride = std::size_t(width) * std::size_t(channels);
    if (raw.size() < (stride + 1) * std::size_t(height)) return false;
    std::vector<std::uint8_t> pixels(stride * std::size_t(height));
    for (int y = 0; y < height; ++y) {
        const int filter = raw[std::size_t(y) * (stride + 1)];
        const auto *src = &raw[std::size_t(y) * (stride + 1) + 1];
        auto *row = &pixels[std::size_t(y) * stride];
        const auto *prev = y ? &pixels[std::size_t(y - 1) * stride] : nullptr;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= std::size_t(channels) ? row[i - std::size_t(channels)] : 0, b = prev ? prev[i] : 0,
                      c = prev && i >= std::size_t(channels) ? prev[i - std::size_t(channels)] : 0;
            int predictor = 0;
            switch (filter) {
            case 1: predictor = a; break;
            case 2: predictor = b; break;
            case 3: predictor = (a + b) / 2; break;
            case 4: {
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                predictor = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                break;
            }
            }
            row[i] = std::uint8_t(src[i] + predictor);
        }
    }
    out.width = width;
    out.height = height;
    out.rgba.resize(std::size_t(width) * std::size_t(height) * 4);
    out.alpha = channels == 4;
    for (std::size_t i = 0; i < std::size_t(width) * std::size_t(height); ++i) {
        const auto *p = &pixels[i * std::size_t(channels)];
        const std::uint8_t alpha = channels == 4 ? p[3] : 255;
        auto *o = &out.rgba[i * 4];
        // BGRA, premultiplied -> RGBA, straight.
        const std::uint8_t r = p[2], g = p[1], bl = p[0];
        if (alpha && alpha < 255) {
            o[0] = std::uint8_t(std::min(255, r * 255 / alpha));
            o[1] = std::uint8_t(std::min(255, g * 255 / alpha));
            o[2] = std::uint8_t(std::min(255, bl * 255 / alpha));
        } else { o[0] = r; o[1] = g; o[2] = bl; }
        o[3] = alpha;
    }
    return true;
}
bool decodeImage(const std::uint8_t *bytes, std::size_t size, ImageData &out) {
    if (decodeCgbi(bytes, size, out)) return true;
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(bytes, int(size), &w, &h, &channels, 4);
    if (!pixels) return false;
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + std::size_t(w) * std::size_t(h) * 4);
    stbi_image_free(pixels);
    out.alpha = false;
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) if (out.rgba[i] != 255) { out.alpha = true; break; }
    return true;
}
id makeImage(ImageData &&data) {
    const auto o = objc::allocInstance(objc::getClass("CGImage"));
    objc::setHostObject(o, std::make_unique<ImageData>(std::move(data)));
    return o;
}
ImageData *image(id o) { return objc::host<ImageData>(o); }

// ---------------------------------------------------------------- fonts
struct Font {
    std::vector<std::uint8_t> bytes;
    stbtt_fontinfo info{};
    int ascent = 0, descent = 0, lineGap = 0, unitsPerEm = 2048;
};
std::shared_ptr<Font> loadFont(std::vector<std::uint8_t> bytes) {
    auto f = std::make_shared<Font>();
    f->bytes = std::move(bytes);
    if (f->bytes.empty() || !stbtt_InitFont(&f->info, f->bytes.data(), stbtt_GetFontOffsetForIndex(f->bytes.data(), 0))) return nullptr;
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->lineGap);
    const int head = int(stbtt__find_table(f->bytes.data(), std::uint32_t(f->info.fontstart), "head"));
    if (head) f->unitsPerEm = ttUSHORT(f->bytes.data() + head + 18);
    return f;
}
// iOS font names -> Windows font files.
std::shared_ptr<Font> systemFont(const std::string &name) {
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<Font>> cache;
    std::string lower = name;
    for (auto &ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    const bool bold = lower.find("bold") != std::string::npos;
    // Font files by preference: the Windows font, then common Linux stand-ins (Liberation has Arial's metrics).
    std::vector<std::string> files;
    if (lower.find("marker") != std::string::npos || lower.find("felt") != std::string::npos) files = bold ? std::vector<std::string>{"comicbd.ttf"} : std::vector<std::string>{"comic.ttf"};
    else if (lower.find("courier") != std::string::npos) files = bold ? std::vector<std::string>{"courbd.ttf", "LiberationMono-Bold.ttf", "DejaVuSansMono-Bold.ttf"} : std::vector<std::string>{"cour.ttf", "LiberationMono-Regular.ttf", "DejaVuSansMono.ttf"};
    else if (lower.find("georgia") != std::string::npos) files = bold ? std::vector<std::string>{"georgiab.ttf"} : std::vector<std::string>{"georgia.ttf"};
    else if (lower.find("times") != std::string::npos) files = bold ? std::vector<std::string>{"timesbd.ttf", "LiberationSerif-Bold.ttf", "DejaVuSerif-Bold.ttf"} : std::vector<std::string>{"times.ttf", "LiberationSerif-Regular.ttf", "DejaVuSerif.ttf"};
    else if (lower.find("verdana") != std::string::npos) files = bold ? std::vector<std::string>{"verdanab.ttf"} : std::vector<std::string>{"verdana.ttf"};
    else if (lower.find("trebuchet") != std::string::npos) files = bold ? std::vector<std::string>{"trebucbd.ttf"} : std::vector<std::string>{"trebuc.ttf"};
    for (const char *f : bold ? std::initializer_list<const char *>{"arialbd.ttf", "LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf", "Ubuntu-B.ttf", "FreeSansBold.ttf"}
                              : std::initializer_list<const char *>{"arial.ttf", "LiberationSans-Regular.ttf", "DejaVuSans.ttf", "Ubuntu-R.ttf", "FreeSans.ttf"})
        files.push_back(f);
    std::lock_guard guard(lock);
    auto &slot = cache[files.front()];
    if (!slot) {
        const auto path = platform::findFont(files);
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        slot = loadFont(std::move(bytes));
        if (!slot) warn("no font found for %s", name.c_str());
    }
    return slot;
}
struct UIFontData : objc::HostObject { std::shared_ptr<Font> font; std::string name; float size = 12; };
struct CGFontData : objc::HostObject { std::shared_ptr<Font> font; std::string name; };

// ---------------------------------------------------------------- bitmap contexts
struct ColorSpaceData : objc::HostObject { bool gray = false; };
struct GState {
    Affine ctm, text;
    float fill[4] = {0, 0, 0, 1}, stroke[4] = {0, 0, 0, 1};
    float lineWidth = 1;
    std::shared_ptr<Font> font;
    float fontSize = 12;
};
struct ContextData : objc::HostObject {
    std::uint64_t data = 0;
    int width = 0, height = 0, bytesPerRow = 0;
    enum Kind { RGBA, BGRA, Gray, Alpha } kind = RGBA;
    bool hasAlpha = true, ownsData = false;
    GState state;
    std::vector<GState> saved;
    std::vector<std::pair<float, float>> path; // the current path (user space), flattened
};
ContextData *context(id c) { return objc::host<ContextData>(c); }
thread_local std::vector<id> t_uiContexts;
id currentUIContext() { return t_uiContexts.empty() ? 0 : t_uiContexts.back(); }

// Composites a colour with coverage onto device pixel (x, y_up), premultiplied in the destination.
void blend(ContextData &c, int x, int yUp, const float rgba[4], float coverage) {
    if (x < 0 || yUp < 0 || x >= c.width || yUp >= c.height || coverage <= 0) return;
    const int row = c.height - 1 - yUp;
    auto *p = mem::base() + c.data + std::size_t(row) * std::size_t(c.bytesPerRow);
    const float a = rgba[3] * coverage;
    auto mix = [&](std::uint8_t &dst, float src) { dst = std::uint8_t(std::clamp(src * a * 255.f + dst * (1 - a), 0.f, 255.f) + 0.5f); };
    switch (c.kind) {
    case ContextData::Gray: {
        const float gray = 0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2];
        mix(p[x], gray);
        break;
    }
    case ContextData::Alpha: p[x] = std::uint8_t(std::clamp(a * 255.f + p[x] * (1 - a), 0.f, 255.f) + 0.5f); break;
    case ContextData::RGBA: {
        auto *px = p + 4 * x;
        mix(px[0], rgba[0]); mix(px[1], rgba[1]); mix(px[2], rgba[2]);
        px[3] = c.hasAlpha ? std::uint8_t(std::clamp(a * 255.f + px[3] * (1 - a), 0.f, 255.f) + 0.5f) : 255;
        break;
    }
    case ContextData::BGRA: {
        auto *px = p + 4 * x;
        mix(px[0], rgba[2]); mix(px[1], rgba[1]); mix(px[2], rgba[0]);
        px[3] = c.hasAlpha ? std::uint8_t(std::clamp(a * 255.f + px[3] * (1 - a), 0.f, 255.f) + 0.5f) : 255;
        break;
    }
    }
}
// The device-space bounding box of a user-space rectangle.
void deviceBox(const ContextData &c, const Rect &r, int &x0, int &y0, int &x1, int &y1) {
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (int i = 0; i < 4; ++i) {
        float ox, oy;
        c.state.ctm.apply(r.x + ((i & 1) ? r.w : 0), r.y + ((i & 2) ? r.h : 0), ox, oy);
        minX = std::min(minX, ox); minY = std::min(minY, oy); maxX = std::max(maxX, ox); maxY = std::max(maxY, oy);
    }
    x0 = std::max(0, int(std::floor(minX + 0.5f)));
    y0 = std::max(0, int(std::floor(minY + 0.5f)));
    x1 = std::min(c.width, int(std::floor(maxX + 0.5f)));
    y1 = std::min(c.height, int(std::floor(maxY + 0.5f)));
}
void clearRect(ContextData &c, const Rect &r) {
    int x0, y0, x1, y1;
    deviceBox(c, r, x0, y0, x1, y1);
    const int bpp = (c.kind == ContextData::Gray || c.kind == ContextData::Alpha) ? 1 : 4;
    for (int y = y0; y < y1; ++y) {
        const int row = c.height - 1 - y;
        std::memset(mem::base() + c.data + std::size_t(row) * std::size_t(c.bytesPerRow) + std::size_t(x0) * std::size_t(bpp), 0,
                    std::size_t(x1 - x0) * std::size_t(bpp));
    }
}
void fillRect(ContextData &c, const Rect &r) {
    int x0, y0, x1, y1;
    deviceBox(c, r, x0, y0, x1, y1);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blend(c, x, y, c.state.fill, 1);
}
// Fills a polygon (user space) with the fill colour: even-odd scanlines at pixel centres.
void fillPolygon(ContextData &c, const std::vector<std::pair<float, float>> &points) {
    if (points.size() < 3) return;
    std::vector<std::pair<float, float>> device;
    float minY = 1e30f, maxY = -1e30f;
    for (auto [x, y] : points) {
        float ox, oy;
        c.state.ctm.apply(x, y, ox, oy);
        device.push_back({ox, oy});
        minY = std::min(minY, oy);
        maxY = std::max(maxY, oy);
    }
    for (int y = std::max(0, int(minY)); y <= std::min(c.height - 1, int(maxY)); ++y) {
        const float sy = float(y) + 0.5f;
        std::vector<float> xs;
        for (std::size_t i = 0; i < device.size(); ++i) {
            const auto [ax, ay] = device[i];
            const auto [bx, by] = device[(i + 1) % device.size()];
            if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) xs.push_back(ax + (sy - ay) / (by - ay) * (bx - ax));
        }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2)
            for (int x = std::max(0, int(std::ceil(xs[i] - 0.5f))); x < std::min(c.width, int(std::ceil(xs[i + 1] - 0.5f))); ++x)
                blend(c, x, y, c.state.fill, 1);
    }
}
std::vector<std::pair<float, float>> ellipse(const Rect &r, float inset = 0) {
    std::vector<std::pair<float, float>> points;
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rx = r.w / 2 - inset, ry = r.h / 2 - inset;
    for (int i = 0; i < 64; ++i) {
        const float t = float(i) * 6.2831853f / 64;
        points.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return points;
}
// Draws an image into a user-space rectangle: each covered device pixel samples the image (bilinear), with the
// image's top row at the rectangle's maximum y.
void drawImage(ContextData &c, const Rect &r, const ImageData &img) {
    if (!img.width || !img.height || r.w == 0 || r.h == 0) return;
    int x0, y0, x1, y1;
    deviceBox(c, r, x0, y0, x1, y1);
    const Affine inverse = c.state.ctm.inverse();
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float ux, uy;
            inverse.apply(float(x) + 0.5f, float(y) + 0.5f, ux, uy);
            const float u = (ux - r.x) / r.w * float(img.width) - 0.5f, v = (1 - (uy - r.y) / r.h) * float(img.height) - 0.5f;
            const int iu = int(std::floor(u)), iv = int(std::floor(v));
            const float fu = u - float(iu), fv = v - float(iv);
            float px[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const int sx = std::clamp(iu + (k & 1), 0, img.width - 1), sy = std::clamp(iv + (k >> 1), 0, img.height - 1);
                const float w = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                const auto *s = &img.rgba[(std::size_t(sy) * std::size_t(img.width) + std::size_t(sx)) * 4];
                const float a = s[3] / 255.f;
                px[0] += s[0] / 255.f * a * w; px[1] += s[1] / 255.f * a * w; px[2] += s[2] / 255.f * a * w; px[3] += a * w;
            }
            if (px[3] <= 0) continue;
            const float color[4] = {px[0] / px[3], px[1] / px[3], px[2] / px[3], px[3]};
            blend(c, x, y, color, 1);
        }
}
// Draws text with its baseline origin at (x, y) in user space. yDown: glyphs grow towards +y (UIKit) or -y (CG).
// Returns the advance width.
float drawText(ContextData *c, const Font &font, float size, const std::u16string &text, float x, float y, bool yDown, const float color[4],
               const Affine &textMatrix = {}) {
    const float scale = stbtt_ScaleForMappingEmToPixels(&font.info, size);
    // Rasterize at the device resolution.
    const float deviceScale = c ? std::max(0.01f, c->state.ctm.scaleY() * textMatrix.scaleY()) : 1;
    float pen = 0;
    int previous = 0;
    for (char16_t ch : text) {
        const int glyph = stbtt_FindGlyphIndex(&font.info, ch);
        if (previous) pen += float(stbtt_GetGlyphKernAdvance(&font.info, previous, glyph)) * scale;
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&font.info, glyph, &advance, &lsb);
        if (c) {
            int w, h, xo, yo;
            const float s = scale * deviceScale;
            unsigned char *bitmap = stbtt_GetGlyphBitmapSubpixel(&font.info, s, s, 0, 0, glyph, &w, &h, &xo, &yo);
            if (bitmap) {
                // Bitmap pixel (i, j) (j down from the top) sits at user (x + pen + (xo + i) / ds, y +- (yo + j) / ds).
                for (int j = 0; j < h; ++j)
                    for (int i = 0; i < w; ++i) {
                        const float coverage = bitmap[j * w + i] / 255.f;
                        if (coverage <= 0) continue;
                        const float lx = (float(xo + i) + 0.5f) / deviceScale, ly = (float(yo + j) + 0.5f) / deviceScale;
                        float tx, ty;
                        textMatrix.apply(lx, yDown ? ly : -ly, tx, ty);
                        float dx, dy;
                        c->state.ctm.apply(x + pen + tx, y + ty, dx, dy);
                        blend(*c, int(std::floor(dx)), int(std::floor(dy)), color, coverage);
                    }
                stbtt_FreeBitmap(bitmap, nullptr);
            }
        }
        pen += float(advance) * scale;
        previous = glyph;
    }
    return pen;
}
float lineHeight(const Font &f, float size) {
    const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, size);
    return float(f.ascent - f.descent + f.lineGap) * scale;
}
float ascender(const Font &f, float size) { return float(f.ascent) * stbtt_ScaleForMappingEmToPixels(&f.info, size); }
float textWidth(const Font &f, float size, const std::u16string &text) { return drawText(nullptr, f, size, text, 0, 0, true, nullptr); }
// Greedy word wrap into lines no wider than width (0: no limit); explicit newlines always break.
std::vector<std::u16string> wrap(const Font &f, float size, const std::u16string &text, float width) {
    std::vector<std::u16string> lines;
    std::u16string paragraph;
    auto flushParagraph = [&](const std::u16string &p) {
        if (width <= 0) { lines.push_back(p); return; }
        std::u16string line;
        std::size_t i = 0;
        while (i <= p.size()) {
            std::size_t j = p.find(u' ', i);
            if (j == std::u16string::npos) j = p.size();
            const std::u16string word = p.substr(i, j - i);
            const std::u16string candidate = line.empty() ? word : line + u" " + word;
            if (!line.empty() && textWidth(f, size, candidate) > width + 0.5f) {
                lines.push_back(line);
                line = word;
            } else line = candidate;
            i = j + 1;
        }
        lines.push_back(line);
    };
    for (char16_t ch : text) {
        if (ch == u'\n') { flushParagraph(paragraph); paragraph.clear(); }
        else if (ch != u'\r') paragraph.push_back(ch);
    }
    flushParagraph(paragraph);
    return lines;
}
UIFontData *uiFont(id f) { return objc::host<UIFontData>(f); }
id makeUIFont(const std::string &name, float size) {
    const auto o = objc::allocInstance(objc::getClass("UIFont"));
    auto d = std::make_unique<UIFontData>();
    d->font = systemFont(name);
    d->name = name;
    d->size = size > 0 ? size : 12;
    objc::setHostObject(o, std::move(d));
    return o;
}
} // namespace

void uikit::drawOverlayBox(int x0, int y0, int x1, int y1, const std::u16string &text, bool dim) {
    int windowWidth = 0, windowHeight = 0;
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    windowWidth = viewport[2];
    windowHeight = viewport[3];
    if (x1 <= x0 || y1 <= y0) return;
    // The box: a 2-pixel grey border around white (scissored clears; GL rows count from the bottom).
    glEnable(GL_SCISSOR_TEST);
    auto box = [&](int l, int t, int r, int b, float grey) {
        glScissor(l, windowHeight - b, r - l, b - t);
        glClearColor(grey, grey, grey, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    box(x0, y0, x1, y1, 0.45f);
    box(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 1.0f);
    glDisable(GL_SCISSOR_TEST);
    auto font = systemFont("Helvetica");
    if (!font || text.empty()) return;
    // Render the line into a top-down RGBA image, then draw it with glDrawPixels (zoomed -1 vertically).
    const float size = float(y1 - y0) * 0.6f;
    const int w = std::min(x1 - x0 - 12, int(textWidth(*font, size, text)) + 4), h = int(lineHeight(*font, size)) + 2;
    if (w <= 0 || h <= 0) return;
    std::vector<std::uint8_t> rgba(std::size_t(w) * std::size_t(h) * 4, 0);
    const float scale = stbtt_ScaleForMappingEmToPixels(&font->info, size);
    float pen = 1;
    const float baseline = ascender(*font, size) + 1;
    const std::uint8_t shade = dim ? 150 : 20;
    for (char16_t ch : text) {
        const int glyph = stbtt_FindGlyphIndex(&font->info, ch);
        int advance, lsb, gw, gh, xo, yo;
        stbtt_GetGlyphHMetrics(&font->info, glyph, &advance, &lsb);
        if (unsigned char *bitmap = stbtt_GetGlyphBitmap(&font->info, scale, scale, glyph, &gw, &gh, &xo, &yo)) {
            for (int j = 0; j < gh; ++j)
                for (int i = 0; i < gw; ++i) {
                    const int x = int(pen) + xo + i, y = int(baseline) + yo + j;
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    auto *p = &rgba[(std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4];
                    p[0] = p[1] = p[2] = shade;
                    p[3] = std::max(p[3], bitmap[j * gw + i]);
                }
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        pen += float(advance) * scale;
    }
    using WindowPos = void(APIENTRY *)(int, int);
    static auto windowPos = reinterpret_cast<WindowPos>(platform::glProc("glWindowPos2i"));
    if (!windowPos) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPixelZoom(1.f, -1.f);
    windowPos(x0 + 6, windowHeight - (y0 + (y1 - y0 - h) / 2));
    glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glPixelZoom(1.f, 1.f);
    glDisable(GL_BLEND);
    (void)windowWidth;
}
void uikit::setFillColor(const float rgba[4]) {
    if (auto *c = context(currentUIContext())) std::copy(rgba, rgba + 4, c->state.fill);
}

OBJC_CLASS(CGColorSpace, NSObject);
OBJC_CLASS(CGContext, NSObject);
OBJC_CLASS(CGImage, NSObject);
OBJC_CLASS(CGDataProvider, NSObject);
OBJC_CLASS(CGFont, NSObject);
OBJC_CLASS(UIFont, NSObject);
OBJC_CLASS(UIImage, NSObject);

// ======================================== Colour spaces and bitmap contexts ========================================
static id makeColorSpace(bool gray) {
    const auto o = objc::allocInstance(objc::getClass("CGColorSpace"));
    auto d = std::make_unique<ColorSpaceData>();
    d->gray = gray;
    objc::setHostObject(o, std::move(d));
    return o;
}
HLE(CGColorSpaceCreateDeviceRGB) { ret(makeColorSpace(false)); }
HLE(CGColorSpaceCreateDeviceGray) { ret(makeColorSpace(true)); }
HLE(CGColorSpaceRelease) {}
// CGBitmapContextCreate(data, width, height, bitsPerComponent, bytesPerRow, space, bitmapInfo). bitmapInfo: alpha info
// in the low 5 bits (none 0, premultiplied last 1, premultiplied first 2, last 3, first 4, none skip last 5, skip first
// 6, only 7), byte order 32 little 0x2000.
HLE(CGBitmapContextCreate) {
    const int width = int(arg(1)), height = int(arg(2));
    int bytesPerRow = int(arg(4));
    const auto space = arg(5);
    const auto info = std::uint32_t(arg(6));
    auto d = std::make_unique<ContextData>();
    const auto alphaInfo = info & 0x1F;
    auto *cs = objc::host<ColorSpaceData>(space);
    if (!space || alphaInfo == 7) d->kind = ContextData::Alpha;
    else if (cs && cs->gray) d->kind = ContextData::Gray;
    else d->kind = (info & 0x7000) == 0x2000 && (alphaInfo == 2 || alphaInfo == 4 || alphaInfo == 6) ? ContextData::BGRA : ContextData::RGBA;
    d->hasAlpha = alphaInfo != 0 && alphaInfo != 5 && alphaInfo != 6;
    const int bpp = d->kind == ContextData::Gray || d->kind == ContextData::Alpha ? 1 : 4;
    if (!bytesPerRow) bytesPerRow = width * bpp;
    d->width = width;
    d->height = height;
    d->bytesPerRow = bytesPerRow;
    d->data = arg(0);
    if (!d->data) {
        d->data = mem::calloc(std::size_t(bytesPerRow) * std::size_t(height));
        d->ownsData = true;
    }
    const auto o = objc::allocInstance(objc::getClass("CGContext"));
    objc::setHostObject(o, std::move(d));
    ret(o);
}
HLE(CGBitmapContextGetData) { auto *c = context(arg(0)); ret(c ? c->data : 0); }
HLE(CGBitmapContextGetWidth) { auto *c = context(arg(0)); ret(c ? std::uint32_t(c->width) : 0); }
HLE(CGBitmapContextGetHeight) { auto *c = context(arg(0)); ret(c ? std::uint32_t(c->height) : 0); }
HLE(CGContextRelease) {}
HLE(CGContextSaveGState) { if (auto *c = context(arg(0))) c->saved.push_back(c->state); }
HLE(CGContextRestoreGState) {
    auto *c = context(arg(0));
    if (c && !c->saved.empty()) { c->state = c->saved.back(); c->saved.pop_back(); }
}
HLE(CGContextTranslateCTM) {
    if (auto *c = context(arg(0))) c->state.ctm = Affine{1, 0, 0, 1, argFloat(1), argFloat(2)}.then(c->state.ctm);
}
HLE(CGContextScaleCTM) {
    if (auto *c = context(arg(0))) c->state.ctm = Affine{argFloat(1), 0, 0, argFloat(2), 0, 0}.then(c->state.ctm);
}
HLE(CGContextRotateCTM) {
    const float a = argFloat(1), co = std::cos(a), s = std::sin(a);
    if (auto *c = context(arg(0))) c->state.ctm = Affine{co, s, -s, co, 0, 0}.then(c->state.ctm);
}
HLE(CGContextSetRGBFillColor) {
    if (auto *c = context(arg(0))) { c->state.fill[0] = argFloat(1); c->state.fill[1] = argFloat(2); c->state.fill[2] = argFloat(3); c->state.fill[3] = argFloat(4); }
}
HLE(CGContextSetRGBStrokeColor) {
    if (auto *c = context(arg(0))) { c->state.stroke[0] = argFloat(1); c->state.stroke[1] = argFloat(2); c->state.stroke[2] = argFloat(3); c->state.stroke[3] = argFloat(4); }
}
HLE(CGContextSetGrayFillColor) {
    if (auto *c = context(arg(0))) { c->state.fill[0] = c->state.fill[1] = c->state.fill[2] = argFloat(1); c->state.fill[3] = argFloat(2); }
}
HLE(CGContextSetFillColorWithColor) {
    auto *c = context(arg(0));
    float rgba[4];
    if (c && colorComponents(arg(1), rgba)) std::copy(rgba, rgba + 4, c->state.fill);
}
HLE(CGContextSetLineWidth) { if (auto *c = context(arg(0))) c->state.lineWidth = argFloat(1); }
HLE(CGContextClearRect) { if (auto *c = context(arg(0))) clearRect(*c, rectAt(1)); }
HLE(CGContextFillRect) { if (auto *c = context(arg(0))) fillRect(*c, rectAt(1)); }
HLE(CGContextFillEllipseInRect) { if (auto *c = context(arg(0))) fillPolygon(*c, ellipse(rectAt(1))); }
// A stroked ellipse: the ring between the outline and the outline inset by the line width.
HLE(CGContextStrokeEllipseInRect) {
    auto *c = context(arg(0));
    if (!c) return;
    const auto r = rectAt(1);
    const float half = c->state.lineWidth / 2;
    const Rect outer{r.x - half, r.y - half, r.w + 2 * half, r.h + 2 * half};
    auto ring = ellipse(outer);
    auto inner = ellipse(outer, c->state.lineWidth);
    std::reverse(inner.begin(), inner.end());
    ring.push_back(ring.front());
    ring.insert(ring.end(), inner.begin(), inner.end());
    ring.push_back(inner.front());
    const auto savedState = c->state;
    std::copy(c->state.stroke, c->state.stroke + 4, c->state.fill);
    fillPolygon(*c, ring);
    c->state = savedState;
}
HLE(CGContextBeginPath) { if (auto *c = context(arg(0))) c->path.clear(); }
HLE(CGContextMoveToPoint) { if (auto *c = context(arg(0))) c->path.push_back({argFloat(1), argFloat(2)}); }
HLE(CGContextAddLineToPoint) { if (auto *c = context(arg(0))) c->path.push_back({argFloat(1), argFloat(2)}); }
// CGContextAddArc(context, x, y, radius, startAngle, endAngle, clockwise)
HLE(CGContextAddArc) {
    auto *c = context(arg(0));
    if (!c) return;
    const float x = argFloat(1), y = argFloat(2), radius = argFloat(3), a0 = argFloat(4);
    float a1 = argFloat(5);
    const bool clockwise = arg(6) != 0;
    if (!clockwise && a1 < a0) a1 += 6.2831853f;
    if (clockwise && a1 > a0) a1 -= 6.2831853f;
    for (int i = 0; i <= 48; ++i) {
        const float t = a0 + (a1 - a0) * float(i) / 48;
        c->path.push_back({x + radius * std::cos(t), y + radius * std::sin(t)});
    }
}
HLE(CGContextClosePath) {}
HLE(CGContextFillPath) {
    auto *c = context(arg(0));
    if (!c) return;
    fillPolygon(*c, c->path);
    c->path.clear();
}
HLE(CGContextDrawImage) {
    auto *c = context(arg(0));
    auto *img = image(arg(5));
    if (c && img) drawImage(*c, rectAt(1), *img);
}
HLE(CGContextSetFont) {
    auto *c = context(arg(0));
    auto *f = objc::host<CGFontData>(arg(1));
    if (c && f) c->state.font = f->font;
}
HLE(CGContextSetFontSize) { if (auto *c = context(arg(0))) c->state.fontSize = argFloat(1); }
HLE(CGContextSetTextDrawingMode) {}
HLE(CGContextSetTextMatrix) {
    if (auto *c = context(arg(0))) c->state.text = {argFloat(1), argFloat(2), argFloat(3), argFloat(4), argFloat(5), argFloat(6)};
}
// CGContextShowGlyphsAtPoint(context, x, y, glyphs, count): TrueType glyph ids in CG (y-up) user space.
HLE(CGContextShowGlyphsAtPoint) {
    auto *c = context(arg(0));
    if (!c || !c->state.font) return;
    const float x = argFloat(1), y = argFloat(2);
    const auto glyphs = arg(3);
    const auto count = arg(4);
    const Font &font = *c->state.font;
    const float scale = stbtt_ScaleForMappingEmToPixels(&font.info, c->state.fontSize);
    const float deviceScale = std::max(0.01f, c->state.ctm.scaleY() * c->state.text.scaleY());
    float pen = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        const int glyph = mem::read<std::uint16_t>(glyphs + 2 * i);
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&font.info, glyph, &advance, &lsb);
        int w, h, xo, yo;
        const float s = scale * deviceScale;
        if (unsigned char *bitmap = stbtt_GetGlyphBitmap(&font.info, s, s, glyph, &w, &h, &xo, &yo)) {
            for (int j = 0; j < h; ++j)
                for (int k = 0; k < w; ++k) {
                    const float coverage = bitmap[j * w + k] / 255.f;
                    if (coverage <= 0) continue;
                    float tx, ty;
                    c->state.text.apply((float(xo + k) + 0.5f) / deviceScale, -(float(yo + j) + 0.5f) / deviceScale, tx, ty);
                    float dx, dy;
                    c->state.ctm.apply(x + pen + tx, y + ty, dx, dy);
                    blend(*c, int(std::floor(dx)), int(std::floor(dy)), c->state.fill, coverage);
                }
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        pen += float(advance) * scale;
    }
}
HLE(UIGraphicsPushContext) { t_uiContexts.push_back(arg(0)); }
HLE(UIGraphicsPopContext) { if (!t_uiContexts.empty()) t_uiContexts.pop_back(); }
HLE(UIGraphicsGetCurrentContext) { ret(currentUIContext()); }
HLE(UIRectFill) { if (auto *c = context(currentUIContext())) fillRect(*c, rectAt(0)); }
HLE(UIRectFillUsingBlendMode) { if (auto *c = context(currentUIContext())) fillRect(*c, rectAt(0)); }

// ======================================== Images ========================================
// CGBitmapContextCreateImage: a copy of the context's pixels.
HLE(CGBitmapContextCreateImage) {
    auto *c = context(arg(0));
    if (!c) return ret(0);
    ImageData img;
    img.width = c->width;
    img.height = c->height;
    img.alpha = c->hasAlpha;
    img.rgba.resize(std::size_t(c->width) * std::size_t(c->height) * 4);
    for (int y = 0; y < c->height; ++y)
        for (int x = 0; x < c->width; ++x) {
            const auto *p = mem::base() + c->data + std::size_t(y) * std::size_t(c->bytesPerRow);
            auto *o = &img.rgba[(std::size_t(y) * std::size_t(c->width) + std::size_t(x)) * 4];
            std::uint8_t r, g, b, a;
            switch (c->kind) {
            case ContextData::Gray: r = g = b = p[x]; a = 255; break;
            case ContextData::Alpha: r = g = b = 0; a = p[x]; break;
            case ContextData::BGRA: b = p[4 * x]; g = p[4 * x + 1]; r = p[4 * x + 2]; a = c->hasAlpha ? p[4 * x + 3] : 255; break;
            default: r = p[4 * x]; g = p[4 * x + 1]; b = p[4 * x + 2]; a = c->hasAlpha ? p[4 * x + 3] : 255; break;
            }
            // Unpremultiply.
            if (a && a < 255) { r = std::uint8_t(std::min(255, r * 255 / a)); g = std::uint8_t(std::min(255, g * 255 / a)); b = std::uint8_t(std::min(255, b * 255 / a)); }
            o[0] = r; o[1] = g; o[2] = b; o[3] = a;
        }
    ret(makeImage(std::move(img)));
}
struct DataProvider : objc::HostObject { std::vector<std::uint8_t> bytes; std::uint64_t guest = 0, size = 0; };
HLE(CGDataProviderCreateWithData) {
    const auto o = objc::allocInstance(objc::getClass("CGDataProvider"));
    auto d = std::make_unique<DataProvider>();
    d->guest = arg(1);
    d->size = arg(2);
    objc::setHostObject(o, std::move(d));
    ret(o);
}
HLE(CGDataProviderCreateWithURL) {
    std::string bytes;
    if (!files::readWholeFile(text(objc::send(arg(0), "path")), bytes)) return ret(0);
    const auto o = objc::allocInstance(objc::getClass("CGDataProvider"));
    auto d = std::make_unique<DataProvider>();
    d->bytes.assign(bytes.begin(), bytes.end());
    objc::setHostObject(o, std::move(d));
    ret(o);
}
HLE(CGDataProviderRelease) {}
// CGImageCreate(width, height, bitsPerComponent, bitsPerPixel, bytesPerRow, space, bitmapInfo, provider, decode,
// shouldInterpolate, intent): 8-bit RGBA / RGBX (premultiplied or not) and 8-bit gray.
HLE(CGImageCreate) {
    const int width = int(arg(0)), height = int(arg(1)), bitsPerPixel = int(arg(3)), bytesPerRow = int(arg(4));
    const auto info = std::uint32_t(arg(6));
    auto *provider = objc::host<DataProvider>(arg(7));
    if (!provider) return ret(0);
    const std::uint8_t *src = provider->guest ? mem::base() + provider->guest : provider->bytes.data();
    ImageData img;
    img.width = width;
    img.height = height;
    const auto alphaInfo = info & 0x1F;
    img.alpha = alphaInfo != 0 && alphaInfo != 5 && alphaInfo != 6;
    img.rgba.resize(std::size_t(width) * std::size_t(height) * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto *p = src + std::size_t(y) * std::size_t(bytesPerRow) + std::size_t(x) * std::size_t(bitsPerPixel / 8);
            auto *o = &img.rgba[(std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4];
            if (bitsPerPixel == 8) { o[0] = o[1] = o[2] = p[0]; o[3] = 255; continue; }
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            o[3] = img.alpha ? p[3] : 255;
            if ((alphaInfo == 1 || alphaInfo == 2) && o[3] && o[3] < 255)
                for (int k = 0; k < 3; ++k) o[k] = std::uint8_t(std::min(255, o[k] * 255 / o[3]));
        }
    ret(makeImage(std::move(img)));
}
HLE(CGImageRelease) {}
HLE(CGImageRetain) { ret(arg(0)); }
HLE(CGImageGetWidth) { auto *i = image(arg(0)); ret(i ? std::uint32_t(i->width) : 0); }
HLE(CGImageGetHeight) { auto *i = image(arg(0)); ret(i ? std::uint32_t(i->height) : 0); }
HLE(CGImageGetBitsPerComponent) { ret(8); }
HLE(CGImageGetBitsPerPixel) { ret(32); }
HLE(CGImageGetBytesPerRow) { auto *i = image(arg(0)); ret(i ? std::uint32_t(i->width * 4) : 0); }
HLE(CGImageGetColorSpace) { ret(makeColorSpace(false)); }
// kCGImageAlphaPremultipliedLast (1) with alpha, kCGImageAlphaNoneSkipLast (5) without.
HLE(CGImageGetAlphaInfo) { auto *i = image(arg(0)); ret(i && i->alpha ? 1 : 5); }

// UIImage: a CGImage and a scale (2 for @2x files).
struct UIImageData : objc::HostObject { id cgImage = 0; float scale = 1; };
static id makeUIImage(id cgImage, float scale, id object = 0) {
    if (!cgImage) return 0;
    const auto o = object ? object : objc::allocInstance(objc::getClass("UIImage"));
    auto d = std::make_unique<UIImageData>();
    d->cgImage = cgImage;
    objc::retain(cgImage);
    d->scale = scale;
    objc::setHostObject(o, std::move(d));
    return o;
}
static id imageFromFile(const std::string &guestPath, id object = 0) {
    std::string bytes;
    if (!files::readWholeFile(guestPath, bytes)) return 0;
    ImageData img;
    if (!decodeImage(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), img)) {
        hlog("image", "could not decode %s", guestPath.c_str());
        return 0;
    }
    const bool retina = guestPath.find("@2x.") != std::string::npos;
    return makeUIImage(makeImage(std::move(img)), retina ? 2.f : 1.f, object);
}
// imageNamed: the bundle resource, preferring the @2x file on the retina screen.
static id imageNamed(const std::string &name) {
    const std::string bundle = files::kBundlePath;
    std::string base = name, ext = "png";
    if (const auto dot = name.rfind('.'); dot != std::string::npos) { base = name.substr(0, dot); ext = name.substr(dot + 1); }
    for (const auto &candidate : {bundle + "/" + base + "@2x." + ext, bundle + "/" + base + "." + ext}) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::u8path(files::hostPath(candidate)), ec)) return imageFromFile(candidate);
    }
    hlog("image", "imageNamed: %s not found", name.c_str());
    return 0;
}
OBJC_CLASS_METHOD(UIImage, "imageNamed:", imageNamed_) { ret(imageNamed(text(marg(0)))); }
OBJC_CLASS_METHOD(UIImage, "imageWithContentsOfFile:", imageWithContentsOfFile) { ret(imageFromFile(text(marg(0)))); }
OBJC_METHOD(UIImage, "initWithContentsOfFile:", initWithContentsOfFile) { ret(imageFromFile(text(marg(0)), self())); }
static id imageFromData(id data, id object = 0) {
    ImageData img;
    if (!data || !decodeImage(mem::base() + dataBytes(data), std::size_t(dataLength(data)), img)) return 0;
    return makeUIImage(makeImage(std::move(img)), 1, object);
}
OBJC_CLASS_METHOD(UIImage, "imageWithData:", imageWithData) { ret(imageFromData(marg(0))); }
OBJC_METHOD(UIImage, "initWithData:", initWithData) { ret(imageFromData(marg(0), self())); }
OBJC_CLASS_METHOD(UIImage, "imageWithCGImage:", imageWithCGImage) { ret(makeUIImage(marg(0), 1)); }
OBJC_METHOD(UIImage, "initWithCGImage:", initWithCGImage) { ret(makeUIImage(marg(0), 1, self())); }
OBJC_METHOD(UIImage, "CGImage", CGImage) { auto *d = objc::host<UIImageData>(self()); ret(d ? d->cgImage : 0); }
OBJC_METHOD(UIImage, "scale", scale) { auto *d = objc::host<UIImageData>(self()); hle::retFloat(d ? d->scale : 1); }
OBJC_METHOD(UIImage, "imageOrientation", imageOrientation) { ret(0); }
OBJC_METHOD(UIImage, "size", size) {
    auto *d = objc::host<UIImageData>(objc::stretSelf());
    auto *i = d ? image(d->cgImage) : nullptr;
    retPoint(i ? float(i->width) / d->scale : 0, i ? float(i->height) / d->scale : 0);
}
// -drawInRect: / -drawAtPoint: into the current UIKit context (y-down user space: flip around the rectangle).
static void drawUIImage(id uiImage, const Rect &r) {
    auto *c = context(currentUIContext());
    auto *d = objc::host<UIImageData>(uiImage);
    auto *i = d ? image(d->cgImage) : nullptr;
    if (!c || !i) return;
    const auto saved = c->state.ctm;
    c->state.ctm = Affine{1, 0, 0, -1, 0, 2 * r.y + r.h}.then(c->state.ctm);
    drawImage(*c, r, *i);
    c->state.ctm = saved;
}
OBJC_METHOD(UIImage, "drawInRect:", drawInRect) { drawUIImage(self(), rectAt(2)); }
OBJC_METHOD(UIImage, "drawAtPoint:", drawAtPoint) {
    auto *d = objc::host<UIImageData>(self());
    auto *i = d ? image(d->cgImage) : nullptr;
    if (i) drawUIImage(self(), {objc::margFloat(0), objc::margFloat(1), float(i->width) / d->scale, float(i->height) / d->scale});
}
HLE(UIImagePNGRepresentation) { hlog("image", "UIImagePNGRepresentation is not implemented"); ret(0); }
HLE(UIImageJPEGRepresentation) { hlog("image", "UIImageJPEGRepresentation is not implemented"); ret(0); }

// ======================================== CGFont ========================================
HLE(CGFontCreateWithDataProvider) {
    auto *p = objc::host<DataProvider>(arg(0));
    if (!p) return ret(0);
    std::vector<std::uint8_t> bytes = p->guest ? std::vector<std::uint8_t>(mem::base() + p->guest, mem::base() + p->guest + p->size) : p->bytes;
    auto font = loadFont(std::move(bytes));
    if (!font) return ret(0);
    const auto o = objc::allocInstance(objc::getClass("CGFont"));
    auto d = std::make_unique<CGFontData>();
    d->font = font;
    objc::setHostObject(o, std::move(d));
    ret(o);
}
HLE(CGFontCreateWithFontName) {
    auto font = systemFont(text(arg(0)));
    if (!font) return ret(0);
    const auto o = objc::allocInstance(objc::getClass("CGFont"));
    auto d = std::make_unique<CGFontData>();
    d->font = font;
    d->name = text(arg(0));
    objc::setHostObject(o, std::move(d));
    ret(o);
}
HLE(CGFontRetain) { ret(arg(0)); }
HLE(CGFontRelease) {}
static Font *cgFont(id f) { auto *d = objc::host<CGFontData>(f); return d && d->font ? d->font.get() : nullptr; }
HLE(CGFontGetUnitsPerEm) { auto *f = cgFont(arg(0)); ret(f ? std::uint32_t(f->unitsPerEm) : 1000); }
HLE(CGFontGetAscent) { auto *f = cgFont(arg(0)); ret(f ? std::uint32_t(f->ascent) : 0); }
HLE(CGFontGetDescent) { auto *f = cgFont(arg(0)); ret(f ? std::uint32_t(f->descent) : 0); }
static int os2Value(Font *f, int offset, int fallback) {
    if (!f) return fallback;
    const auto table = stbtt__find_table(f->bytes.data(), std::uint32_t(f->info.fontstart), "OS/2");
    if (!table || ttUSHORT(f->bytes.data() + table) < 2) return fallback;
    return ttSHORT(f->bytes.data() + table + offset);
}
HLE(CGFontGetCapHeight) { auto *f = cgFont(arg(0)); ret(std::uint32_t(os2Value(f, 88, f ? f->ascent * 7 / 10 : 0))); }
HLE(CGFontGetXHeight) { auto *f = cgFont(arg(0)); ret(std::uint32_t(os2Value(f, 86, f ? f->ascent / 2 : 0))); }
// CGFontGetGlyphAdvances(font, glyphs, count, advances (int, font units))
HLE(CGFontGetGlyphAdvances) {
    auto *f = cgFont(arg(0));
    if (!f) return ret(0);
    for (std::uint64_t i = 0; i < arg(2); ++i) {
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&f->info, mem::read<std::uint16_t>(arg(1) + 2 * i), &advance, &lsb);
        mem::w32(arg(3) + 4 * i, std::uint32_t(advance));
    }
    ret(1);
}
// CGFontCopyTableForTag(font, tag): the raw TrueType table as CFData (FontLabel reads 'cmap' to map characters).
HLE(CGFontCopyTableForTag) {
    auto *f = cgFont(arg(0));
    if (!f) return ret(0);
    const auto tag = std::uint32_t(arg(1));
    const char name[5] = {char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag), 0};
    std::uint8_t *data = f->bytes.data();
    const int fontStart = f->info.fontstart;
    const int tables = ttUSHORT(data + fontStart + 4);
    for (int i = 0; i < tables; ++i) {
        auto *entry = data + fontStart + 12 + 16 * i;
        if (std::memcmp(entry, name, 4) == 0) {
            const auto offset = ttULONG(entry + 8), length = ttULONG(entry + 12);
            return ret(ns::data(data + offset, length));
        }
    }
    ret(0);
}

// ======================================== UIFont ========================================
OBJC_CLASS_METHOD(UIFont, "fontWithName:size:", fontWithName) { ret(makeUIFont(text(marg(0)), objc::margFloat(1))); }
OBJC_CLASS_METHOD(UIFont, "systemFontOfSize:", systemFontOfSize) { ret(makeUIFont("Helvetica", objc::margFloat(0))); }
OBJC_CLASS_METHOD(UIFont, "boldSystemFontOfSize:", boldSystemFontOfSize) { ret(makeUIFont("Helvetica-Bold", objc::margFloat(0))); }
OBJC_CLASS_METHOD(UIFont, "italicSystemFontOfSize:", italicSystemFontOfSize) { ret(makeUIFont("Helvetica-Oblique", objc::margFloat(0))); }
OBJC_CLASS_METHOD(UIFont, "systemFontSize", systemFontSize) { hle::retFloat(14); }
OBJC_CLASS_METHOD(UIFont, "familyNames", familyNames) { ret(array({string("Helvetica"), string("Arial"), string("Marker Felt")})); }
OBJC_CLASS_METHOD(UIFont, "fontNamesForFamilyName:", fontNamesForFamilyName) { ret(array({marg(0)})); }
OBJC_METHOD(UIFont, "fontWithSize:", fontWithSize) { auto *d = uiFont(self()); ret(makeUIFont(d ? d->name : "Helvetica", objc::margFloat(0))); }
OBJC_METHOD(UIFont, "pointSize", pointSize) { auto *d = uiFont(self()); hle::retFloat(d ? d->size : 12); }
OBJC_METHOD(UIFont, "fontName", fontName) { auto *d = uiFont(self()); ret(string(d ? d->name : "Helvetica")); }
OBJC_METHOD(UIFont, "familyName", familyName) { auto *d = uiFont(self()); ret(string(d ? d->name : "Helvetica")); }
OBJC_METHOD(UIFont, "ascender", ascender_) { auto *d = uiFont(self()); hle::retFloat(d && d->font ? ascender(*d->font, d->size) : 0); }
OBJC_METHOD(UIFont, "descender", descender) {
    auto *d = uiFont(self());
    hle::retFloat(d && d->font ? float(d->font->descent) * stbtt_ScaleForMappingEmToPixels(&d->font->info, d->size) : 0);
}
OBJC_METHOD(UIFont, "lineHeight", lineHeight_) { auto *d = uiFont(self()); hle::retFloat(d && d->font ? lineHeight(*d->font, d->size) : 0); }
OBJC_METHOD(UIFont, "leading", leading) { auto *d = uiFont(self()); hle::retFloat(d && d->font ? lineHeight(*d->font, d->size) : 0); }
OBJC_METHOD(UIFont, "capHeight", capHeight) { auto *d = uiFont(self()); hle::retFloat(d ? d->size * 0.7f : 0); }
OBJC_METHOD(UIFont, "xHeight", xHeight) { auto *d = uiFont(self()); hle::retFloat(d ? d->size * 0.5f : 0); }

// ======================================== NSString (UIStringDrawing) ========================================
// The size of text laid out within width (0: unlimited) with a font.
static void measure(id string, id font, float width, float &w, float &h) {
    w = h = 0;
    auto *f = uiFont(font);
    if (!f || !f->font) return;
    const auto lines = wrap(*f->font, f->size, text16(string), width);
    for (const auto &line : lines) w = std::max(w, textWidth(*f->font, f->size, line));
    h = lineHeight(*f->font, f->size) * float(lines.size());
    if (width > 0) w = std::min(w, width);
}
// Results are CGSize: objc_msgSend_stret (result r0, self r1, _cmd r2, arguments from r3).
OBJC_METHOD(NSString, "sizeWithFont:", sizeWithFont) {
    float w, h;
    measure(objc::stretSelf(), objc::stretArg(0), 0, w, h);
    retPoint(std::ceil(w), std::ceil(h));
}
OBJC_METHOD(NSString, "sizeWithFont:constrainedToSize:", sizeWithFontConstrained) {
    float w, h;
    measure(objc::stretSelf(), objc::stretArg(0), objc::stretArgFloat(1), w, h);
    retPoint(std::ceil(w), std::ceil(std::min(h, objc::stretArgFloat(2))));
}
OBJC_METHOD(NSString, "sizeWithFont:constrainedToSize:lineBreakMode:", sizeWithFontConstrainedMode) {
    float w, h;
    measure(objc::stretSelf(), objc::stretArg(0), objc::stretArgFloat(1), w, h);
    retPoint(std::ceil(w), std::ceil(std::min(h, objc::stretArgFloat(2))));
}
OBJC_METHOD(NSString, "sizeWithFont:forWidth:lineBreakMode:", sizeWithFontForWidth) {
    float w, h;
    measure(objc::stretSelf(), objc::stretArg(0), 0, w, h);
    auto *f = uiFont(objc::stretArg(0));
    retPoint(std::ceil(std::min(w, objc::stretArgFloat(1))), f && f->font ? std::ceil(lineHeight(*f->font, f->size)) : 0);
}
// Draws text in the current UIKit context, lines top-down from rect.y, aligned in rect (UITextAlignment left 0,
// center 1, right 2).
static void drawString(id string, const Rect &rect, id font, int alignment, float &usedW, float &usedH) {
    usedW = usedH = 0;
    auto *f = uiFont(font);
    if (!f || !f->font) return;
    auto *c = context(currentUIContext());
    const auto lines = wrap(*f->font, f->size, text16(string), rect.w);
    const float lh = lineHeight(*f->font, f->size), asc = ascender(*f->font, f->size);
    float y = rect.y;
    for (const auto &line : lines) {
        if (rect.h > 0 && y + lh > rect.y + rect.h + 0.5f && y > rect.y) break;
        const float lw = textWidth(*f->font, f->size, line);
        float x = rect.x;
        if (alignment == 1) x += (rect.w - lw) / 2;
        else if (alignment == 2) x += rect.w - lw;
        if (c) drawText(c, *f->font, f->size, line, x, y + asc, true, c->state.fill);
        usedW = std::max(usedW, lw);
        y += lh;
        usedH += lh;
    }
}
OBJC_METHOD(NSString, "drawInRect:withFont:lineBreakMode:alignment:", drawInRectAligned) {
    float w, h;
    const Rect r{objc::stretArgFloat(0), objc::stretArgFloat(1), objc::stretArgFloat(2), objc::stretArgFloat(3)};
    drawString(objc::stretSelf(), r, objc::stretArg(4), int(objc::stretArg(6)), w, h);
    retPoint(w, h);
}
OBJC_METHOD(NSString, "drawInRect:withFont:lineBreakMode:", drawInRectMode) {
    float w, h;
    const Rect r{objc::stretArgFloat(0), objc::stretArgFloat(1), objc::stretArgFloat(2), objc::stretArgFloat(3)};
    drawString(objc::stretSelf(), r, objc::stretArg(4), 0, w, h);
    retPoint(w, h);
}
OBJC_METHOD(NSString, "drawInRect:withFont:", drawInRect) {
    float w, h;
    const Rect r{objc::stretArgFloat(0), objc::stretArgFloat(1), objc::stretArgFloat(2), objc::stretArgFloat(3)};
    drawString(objc::stretSelf(), r, objc::stretArg(4), 0, w, h);
    retPoint(w, h);
}
OBJC_METHOD(NSString, "drawAtPoint:withFont:", drawAtPoint) {
    float w, h;
    drawString(objc::stretSelf(), {objc::stretArgFloat(0), objc::stretArgFloat(1), 0, 0}, objc::stretArg(2), 0, w, h);
    retPoint(w, h);
}
OBJC_METHOD(NSString, "drawAtPoint:forWidth:withFont:lineBreakMode:", drawAtPointForWidth) {
    float w, h;
    drawString(objc::stretSelf(), {objc::stretArgFloat(0), objc::stretArgFloat(1), 0, 0}, objc::stretArg(3), 0, w, h);
    retPoint(w, h);
}
