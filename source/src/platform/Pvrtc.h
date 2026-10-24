#pragma once
// PVRTC (PowerVR texture compression) decoding for GL_IMG_texture_compression_pvrtc textures, which desktop GPUs
// cannot sample: 4 bpp (4x4 pixel blocks) and 2 bpp (8x4 pixel blocks), to RGBA8.
#include <cstdint>
#include <vector>

namespace pvrtc {
// data: the blocks in Morton (twiddled) order, 8 bytes each (modulation word, then colour word).
std::vector<std::uint8_t> decode(const std::uint8_t *data, int width, int height, bool twoBpp);
} // namespace pvrtc
