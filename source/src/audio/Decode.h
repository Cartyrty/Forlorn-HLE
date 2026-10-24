#pragma once
// Decodes the app's audio files to 16-bit interleaved PCM: CAF (AAC, IMA4, linear PCM), AIFF / AIFC (PCM, sowt,
// fl32, ima4), WAV, and MP3 / AAC through the host's codecs (platform/MediaDecode.h). Results are cached by host path.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio {
struct Pcm {
    std::vector<std::int16_t> samples; // interleaved
    int channels = 2, rate = 44100;
    std::uint64_t frames() const { return channels ? samples.size() / std::size_t(channels) : 0; }
};
std::shared_ptr<const Pcm> decodeFile(const std::string &hostPath);
} // namespace audio
