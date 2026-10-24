#pragma once
// Compressed audio files (MP3, ADTS AAC) decoded to 16-bit stereo 44.1 kHz PCM by the host's codecs: Media Foundation
// on Windows, FFmpeg (libavformat / libavcodec / libswresample) on Linux.
#include <cstdint>
#include <string>
#include <vector>

namespace platform {
bool decodeMediaFile(const std::string &hostPath, std::vector<std::int16_t> &stereoSamples);
} // namespace platform
