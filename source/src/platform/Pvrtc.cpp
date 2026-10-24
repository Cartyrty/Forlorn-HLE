#include "platform/Pvrtc.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pvrtc {
namespace {
struct Colour { float r, g, b, a; };
// Colour A: bits 0-15 of the colour word (bit 0 is the modulation mode); colour B: bits 16-31. Bit 15 / 31 selects
// opaque RGB 554 / 555 or translucent ARGB 3443 / 3444. Returned as 0-255.
Colour colourA(std::uint32_t w) {
    if (w & 0x8000) {
        const unsigned r = (w >> 10) & 31, g = (w >> 5) & 31, b4 = (w >> 1) & 15;
        const unsigned b = (b4 << 1) | (b4 >> 3);
        return {float((r << 3) | (r >> 2)), float((g << 3) | (g >> 2)), float((b << 3) | (b >> 2)), 255.f};
    }
    const unsigned a = (w >> 12) & 7, r = (w >> 8) & 15, g = (w >> 4) & 15, b3 = (w >> 1) & 7;
    const unsigned b = (b3 << 1) | (b3 >> 2);
    return {float((r << 4) | r), float((g << 4) | g), float((b << 4) | b), float(a) * 255.f / 7.f};
}
Colour colourB(std::uint32_t w) {
    if (w & 0x80000000u) {
        const unsigned r = (w >> 26) & 31, g = (w >> 21) & 31, b = (w >> 16) & 31;
        return {float((r << 3) | (r >> 2)), float((g << 3) | (g >> 2)), float((b << 3) | (b >> 2)), 255.f};
    }
    const unsigned a = (w >> 28) & 7, r = (w >> 24) & 15, g = (w >> 20) & 15, b = (w >> 16) & 15;
    return {float((r << 4) | r), float((g << 4) | g), float((b << 4) | b), float(a) * 255.f / 7.f};
}
// Morton order of the blocks (the smaller dimension's bits interleaved, Y in the lower bit; extra bits of the larger
// dimension above them).
std::uint32_t twiddle(std::uint32_t xSize, std::uint32_t ySize, std::uint32_t x, std::uint32_t y) {
    std::uint32_t minimum = xSize, maximum = y, twiddled = 0, src = 1, dst = 1;
    int shift = 0;
    if (ySize < xSize) { minimum = ySize; maximum = x; }
    while (src < minimum) {
        if (y & src) twiddled |= dst;
        if (x & src) twiddled |= dst << 1;
        src <<= 1;
        dst <<= 2;
        ++shift;
    }
    return twiddled | ((maximum >> shift) << (2 * shift));
}
} // namespace

std::vector<std::uint8_t> decode(const std::uint8_t *data, int width, int height, bool twoBpp) {
    const int bw = twoBpp ? 8 : 4, bh = 4;
    const int bx = std::max(width / bw, 2), by = std::max(height / bh, 2);
    struct Block { std::uint32_t mod, colour; };
    std::vector<Block> blocks(std::size_t(bx) * std::size_t(by));
    for (int y = 0; y < by; ++y)
        for (int x = 0; x < bx; ++x) {
            const auto index = twiddle(std::uint32_t(bx), std::uint32_t(by), std::uint32_t(x), std::uint32_t(y));
            Block b;
            std::memcpy(&b.mod, data + 8 * std::size_t(index), 4);
            std::memcpy(&b.colour, data + 8 * std::size_t(index) + 4, 4);
            blocks[std::size_t(y) * std::size_t(bx) + std::size_t(x)] = b;
        }
    auto block = [&](int x, int y) -> const Block & {
        x = ((x % bx) + bx) % bx;
        y = ((y % by) + by) % by;
        return blocks[std::size_t(y) * std::size_t(bx) + std::size_t(x)];
    };
    // The 2 bpp interpolated mode stores every other pixel; the rest take the average of their neighbours.
    auto modulation = [&](const Block &b, int px, int py, bool &punch) -> float {
        punch = false;
        if (!twoBpp) {
            const unsigned code = (b.mod >> (2 * (py * 4 + px))) & 3;
            if (b.colour & 1) { // punch-through mode
                if (code == 2) punch = true;
                static const float pt[4] = {0, 4, 4, 8};
                return pt[code] / 8.f;
            }
            static const float std4[4] = {0, 3, 5, 8};
            return std4[code] / 8.f;
        }
        if (!(b.colour & 1)) return float((b.mod >> (py * 8 + px)) & 1);
        static const float std4[4] = {0, 3, 5, 8};
        auto stored = [&](int x, int y) { return std4[(b.mod >> (2 * ((y * 8 + x) / 2))) & 3] / 8.f; };
        if (((px ^ py) & 1) == 0) return stored(px, py);
        const int left = std::max(px - 1, 0), right = std::min(px + 1, 7);
        return (stored(left, py) + stored(right, py)) / 2.f;
    };
    std::vector<std::uint8_t> out(std::size_t(width) * std::size_t(height) * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            // Block colours are defined at block centres: bilinear between the four surrounding blocks.
            const int ox = x - bw / 2, oy = y - bh / 2;
            const int x0 = ox >= 0 ? ox / bw : -((-ox + bw - 1) / bw), y0 = oy >= 0 ? oy / bh : -((-oy + bh - 1) / bh);
            const float tx = float(ox - x0 * bw) / float(bw), ty = float(oy - y0 * bh) / float(bh);
            const Block &p = block(x0, y0), &q = block(x0 + 1, y0), &r = block(x0, y0 + 1), &s = block(x0 + 1, y0 + 1);
            auto mix = [&](Colour (*f)(std::uint32_t)) {
                const Colour a = f(p.colour), b = f(q.colour), c = f(r.colour), d = f(s.colour);
                auto lerp2 = [&](float va, float vb, float vc, float vd) {
                    return (va * (1 - tx) + vb * tx) * (1 - ty) + (vc * (1 - tx) + vd * tx) * ty;
                };
                return Colour{lerp2(a.r, b.r, c.r, d.r), lerp2(a.g, b.g, c.g, d.g), lerp2(a.b, b.b, c.b, d.b), lerp2(a.a, b.a, c.a, d.a)};
            };
            const Colour ca = mix(colourA), cb = mix(colourB);
            const Block &own = block(x / bw, y / bh);
            bool punch = false;
            const float m = modulation(own, x % bw, y % bh, punch);
            auto *px = &out[(std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4];
            px[0] = std::uint8_t(std::lround(std::clamp(ca.r + (cb.r - ca.r) * m, 0.f, 255.f)));
            px[1] = std::uint8_t(std::lround(std::clamp(ca.g + (cb.g - ca.g) * m, 0.f, 255.f)));
            px[2] = std::uint8_t(std::lround(std::clamp(ca.b + (cb.b - ca.b) * m, 0.f, 255.f)));
            px[3] = punch ? 0 : std::uint8_t(std::lround(std::clamp(ca.a + (cb.a - ca.a) * m, 0.f, 255.f)));
        }
    return out;
}
} // namespace pvrtc
