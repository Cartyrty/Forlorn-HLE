#include "audio/Decode.h"
#include "libc/Files.h"
#include "Log.h"
#include "platform/MediaDecode.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace audio {
namespace {
std::uint32_t be32(const std::string &b, std::size_t o) {
    return (std::uint32_t(std::uint8_t(b[o])) << 24) | (std::uint32_t(std::uint8_t(b[o + 1])) << 16) | (std::uint32_t(std::uint8_t(b[o + 2])) << 8) |
           std::uint8_t(b[o + 3]);
}
std::uint16_t be16(const std::string &b, std::size_t o) { return std::uint16_t((std::uint8_t(b[o]) << 8) | std::uint8_t(b[o + 1])); }
std::uint64_t be64(const std::string &b, std::size_t o) { return (std::uint64_t(be32(b, o)) << 32) | be32(b, o + 4); }
std::uint32_t le32(const std::string &b, std::size_t o) { std::uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; }
std::uint16_t le16(const std::string &b, std::size_t o) { std::uint16_t v; std::memcpy(&v, b.data() + o, 2); return v; }

// ---------------------------------------------------------------- IMA ADPCM
const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStepTable[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
                            107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
                            796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
                            4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
                            20350, 22385, 24623, 27086, 29794, 32767};
std::int16_t imaStep(int nibble, int &predictor, int &index) {
    const int step = kStepTable[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    predictor = std::clamp(predictor + diff, -32768, 32767);
    index = std::clamp(index + kIndexTable[nibble & 15], 0, 88);
    return std::int16_t(predictor);
}
// Apple IMA4: 34-byte packets per channel (2-byte header: predictor top 9 bits, step index low 7 bits; then 64
// nibbles, low nibble first); channels' packets alternate.
void decodeAppleIma4(const unsigned char *data, std::size_t size, int channels, Pcm &out) {
    const std::size_t frameBytes = std::size_t(34 * channels);
    for (std::size_t at = 0; at + frameBytes <= size; at += frameBytes) {
        std::int16_t samples[2][64];
        for (int c = 0; c < channels; ++c) {
            const auto *p = data + at + std::size_t(34 * c);
            const int header = (p[0] << 8) | p[1];
            int predictor = std::int16_t(header & 0xFF80), index = std::clamp(header & 0x7F, 0, 88);
            for (int i = 0; i < 32; ++i) {
                samples[c][2 * i] = imaStep(p[2 + i] & 15, predictor, index);
                samples[c][2 * i + 1] = imaStep(p[2 + i] >> 4, predictor, index);
            }
        }
        for (int i = 0; i < 64; ++i)
            for (int c = 0; c < channels; ++c) out.samples.push_back(samples[c][i]);
    }
}
// Linear PCM in any of the common layouts to 16-bit.
void decodeLinear(const unsigned char *data, std::size_t size, int bits, bool littleEndian, bool isFloat, Pcm &out) {
    const std::size_t step = std::size_t(bits / 8);
    for (std::size_t at = 0; at + step <= size; at += step) {
        const auto *p = data + at;
        if (isFloat && bits == 32) {
            std::uint32_t raw = littleEndian ? (p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24))
                                             : ((std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
            float f;
            std::memcpy(&f, &raw, 4);
            out.samples.push_back(std::int16_t(std::lround(std::clamp(f, -1.f, 1.f) * 32767.f)));
        } else if (bits == 8) {
            out.samples.push_back(std::int16_t(std::int8_t(p[0]) << 8)); // AIFF/CAF 8-bit is signed
        } else {
            // Keep the top 16 bits.
            const int hi = littleEndian ? p[step - 1] : p[0], lo = littleEndian ? p[step - 2] : p[1];
            out.samples.push_back(std::int16_t((hi << 8) | lo));
        }
    }
}

// ---------------------------------------------------------------- MP3, ADTS AAC (the host's codecs)
bool decodeMedia(const std::string &hostPath, Pcm &out) {
    out.channels = 2;
    out.rate = 44100;
    return platform::decodeMediaFile(hostPath, out.samples) && !out.samples.empty();
}

// ---------------------------------------------------------------- CAF
// AAC in CAF: the packets (sizes from 'pakt') get ADTS headers built from the AudioSpecificConfig in 'kuki' (esds), and
// the resulting .aac stream is decoded by the host's AAC decoder; priming frames are trimmed.
std::size_t readVarint(const std::string &b, std::size_t &at) {
    std::size_t v = 0;
    for (;;) {
        const auto byte = std::uint8_t(b[at++]);
        v = (v << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) return v;
    }
}
bool audioSpecificConfig(const std::string &esds, int &objectType, int &rateIndex, int &channelConfig) {
    // Find DecoderSpecificInfo (tag 0x05): tag, length (1-4 bytes, 0x80 continuation), then the config.
    for (std::size_t i = 0; i + 2 < esds.size(); ++i) {
        if (std::uint8_t(esds[i]) != 0x05) continue;
        std::size_t at = i + 1, length = 0;
        for (int k = 0; k < 4 && at < esds.size(); ++k) {
            const auto byte = std::uint8_t(esds[at++]);
            length = (length << 7) | (byte & 0x7F);
            if (!(byte & 0x80)) break;
        }
        if (length < 2 || at + 2 > esds.size()) continue;
        const int b0 = std::uint8_t(esds[at]), b1 = std::uint8_t(esds[at + 1]);
        objectType = b0 >> 3;
        rateIndex = ((b0 & 7) << 1) | (b1 >> 7);
        channelConfig = (b1 >> 3) & 15;
        return objectType > 0 && rateIndex < 13;
    }
    return false;
}
bool decodeCafAac(const std::string &b, const std::string &kuki, std::size_t dataAt, std::size_t dataSize, const std::string &pakt,
                  int channels, const std::string &hostPath, Pcm &out) {
    int objectType = 2, rateIndex = 4, channelConfig = channels;
    if (!audioSpecificConfig(kuki, objectType, rateIndex, channelConfig)) hlog("audio", "%s: no AudioSpecificConfig; assuming AAC LC", hostPath.c_str());
    if (pakt.size() < 24) return false;
    const auto packets = be64(pakt, 0), validFrames = be64(pakt, 8);
    const auto priming = be32(pakt, 16);
    std::string adts;
    std::size_t at = 24, offset = dataAt;
    for (std::uint64_t p = 0; p < packets && at < pakt.size(); ++p) {
        const auto size = readVarint(pakt, at);
        if (offset + size > dataAt + dataSize) break;
        const std::size_t frameLength = size + 7;
        unsigned char h[7];
        h[0] = 0xFF;
        h[1] = 0xF1; // MPEG-4, layer 0, no CRC
        h[2] = std::uint8_t(((objectType - 1) << 6) | (rateIndex << 2) | (channelConfig >> 2));
        h[3] = std::uint8_t(((channelConfig & 3) << 6) | (frameLength >> 11));
        h[4] = std::uint8_t((frameLength >> 3) & 0xFF);
        h[5] = std::uint8_t(((frameLength & 7) << 5) | 0x1F);
        h[6] = 0xFC;
        adts.append(reinterpret_cast<char *>(h), 7);
        adts.append(b, offset, size);
        offset += size;
    }
    // The host's decoder reads a file: a cached .aac next to the sandbox.
    const auto cacheDir = std::filesystem::u8path(files::sandboxRoot()) / "audio-cache";
    std::filesystem::create_directories(cacheDir);
    const auto aacPath = cacheDir / (std::filesystem::u8path(hostPath).stem().u8string() + u8".aac");
    {
        std::ofstream file(aacPath, std::ios::binary);
        file.write(adts.data(), std::streamsize(adts.size()));
    }
    Pcm decoded;
    const auto aacUtf8 = aacPath.u8string();
    if (!decodeMedia(std::string(aacUtf8.begin(), aacUtf8.end()), decoded)) return false;
    // Trim the encoder delay (priming frames) and keep the valid frames (the decoder output is 44.1 kHz stereo).
    const std::size_t skip = std::size_t(priming) * 2, keep = validFrames ? std::size_t(validFrames) * 2 : decoded.samples.size();
    if (skip < decoded.samples.size()) decoded.samples.erase(decoded.samples.begin(), decoded.samples.begin() + std::ptrdiff_t(skip));
    if (decoded.samples.size() > keep) decoded.samples.resize(keep);
    out = std::move(decoded);
    return true;
}
bool decodeCaf(const std::string &b, const std::string &hostPath, Pcm &out) {
    if (b.size() < 8 || b.compare(0, 4, "caff")) return false;
    std::string format, kuki, pakt;
    std::uint32_t flags = 0, bytesPerPacket = 0, framesPerPacket = 0, bits = 0;
    double rate = 44100;
    std::size_t dataAt = 0, dataSize = 0;
    for (std::size_t at = 8; at + 12 <= b.size();) {
        const auto type = b.substr(at, 4);
        auto size = std::int64_t(be64(b, at + 4));
        const std::size_t body = at + 12;
        if (size < 0) size = std::int64_t(b.size() - body); // data chunk of unknown size runs to the end
        if (type == "desc") {
            const auto raw = be64(b, body);
            std::memcpy(&rate, &raw, 8);
            format = b.substr(body + 8, 4);
            flags = be32(b, body + 12);
            bytesPerPacket = be32(b, body + 16);
            framesPerPacket = be32(b, body + 20);
            out.channels = int(be32(b, body + 24));
            bits = be32(b, body + 28);
        } else if (type == "kuki") kuki = b.substr(body, std::size_t(size));
        else if (type == "pakt") pakt = b.substr(body, std::size_t(size));
        else if (type == "data") { dataAt = body + 4; dataSize = std::size_t(size) - 4; } // 4-byte edit count first
        at = body + std::size_t(size);
    }
    (void)bytesPerPacket;
    (void)framesPerPacket;
    out.rate = int(rate);
    if (!dataAt || out.channels < 1 || out.channels > 2) return false;
    dataSize = std::min(dataSize, b.size() - dataAt);
    const auto *data = reinterpret_cast<const unsigned char *>(b.data()) + dataAt;
    if (format == "lpcm") {
        decodeLinear(data, dataSize, int(bits), (flags & 2) != 0, (flags & 1) != 0, out);
        return true;
    }
    if (format == "ima4") {
        decodeAppleIma4(data, dataSize, out.channels, out);
        return true;
    }
    if (format == "aac ") return decodeCafAac(b, kuki, dataAt, dataSize, pakt, out.channels, hostPath, out);
    hlog("audio", "%s: unsupported CAF format '%s'", hostPath.c_str(), format.c_str());
    return false;
}

// ---------------------------------------------------------------- AIFF / AIFC
double extended80(const std::string &b, std::size_t o) {
    const int exponent = ((std::uint8_t(b[o]) & 0x7F) << 8) | std::uint8_t(b[o + 1]);
    const auto mantissa = be64(b, o + 2);
    return std::ldexp(double(mantissa), exponent - 16383 - 63);
}
bool decodeAiff(const std::string &b, Pcm &out) {
    if (b.size() < 12 || b.compare(0, 4, "FORM")) return false;
    const bool aifc = b.compare(8, 4, "AIFC") == 0;
    int bits = 16;
    std::string compression = "NONE";
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const auto type = b.substr(at, 4);
        const auto size = be32(b, at + 4);
        const std::size_t body = at + 8;
        if (type == "COMM") {
            out.channels = be16(b, body);
            bits = be16(b, body + 6);
            out.rate = int(extended80(b, body + 8));
            if (aifc && size >= 22) compression = b.substr(body + 18, 4);
        } else if (type == "SSND") {
            const auto offset = be32(b, body);
            const std::size_t start = body + 8 + offset, end = std::min<std::size_t>(b.size(), body + size);
            if (start >= end) return false;
            const auto *data = reinterpret_cast<const unsigned char *>(b.data()) + start;
            if (compression == "NONE" || compression == "twos") decodeLinear(data, end - start, bits, false, false, out);
            else if (compression == "sowt") decodeLinear(data, end - start, bits, true, false, out);
            else if (compression == "fl32" || compression == "FL32") decodeLinear(data, end - start, 32, false, true, out);
            else if (compression == "ima4") decodeAppleIma4(data, end - start, out.channels, out);
            else { hlog("audio", "unsupported AIFC compression '%s'", compression.c_str()); return false; }
        }
        at = body + size + (size & 1);
    }
    return !out.samples.empty();
}

// ---------------------------------------------------------------- WAV (16-bit / 8-bit PCM)
bool decodeWav(const std::string &b, Pcm &out) {
    if (b.size() < 12 || b.compare(0, 4, "RIFF") || b.compare(8, 4, "WAVE")) return false;
    int bits = 16, format = 1;
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const auto type = b.substr(at, 4);
        const auto size = le32(b, at + 4);
        if (type == "fmt ") {
            format = le16(b, at + 8);
            out.channels = le16(b, at + 10);
            out.rate = int(le32(b, at + 12));
            bits = le16(b, at + 22);
        } else if (type == "data" && format == 1) {
            const auto end = std::min<std::size_t>(b.size(), at + 8 + size);
            const auto *data = reinterpret_cast<const unsigned char *>(b.data()) + at + 8;
            if (bits == 8) for (std::size_t i = 0; i < end - at - 8; ++i) out.samples.push_back(std::int16_t((data[i] - 128) << 8));
            else decodeLinear(data, end - at - 8, bits, true, false, out);
        }
        at += 8 + size + (size & 1);
    }
    return !out.samples.empty();
}
} // namespace

std::shared_ptr<const Pcm> decodeFile(const std::string &hostPath) {
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<const Pcm>> cache;
    {
        std::lock_guard guard(lock);
        if (auto found = cache.find(hostPath); found != cache.end()) return found->second;
    }
    std::ifstream in(std::filesystem::u8path(hostPath), std::ios::binary);
    if (!in) return nullptr;
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    auto pcm = std::make_shared<Pcm>();
    bool ok = decodeCaf(bytes, hostPath, *pcm) || decodeAiff(bytes, *pcm) || decodeWav(bytes, *pcm);
    if (!ok) {
        *pcm = Pcm{};
        ok = decodeMedia(hostPath, *pcm);
    }
    if (!ok) {
        hlog("audio", "could not decode %s", hostPath.c_str());
        return nullptr;
    }
    const auto name = std::filesystem::u8path(hostPath).filename().u8string();
    hlog("audio", "decoded %s: %d Hz, %d ch, %.2f s", std::string(name.begin(), name.end()).c_str(), pcm->rate, pcm->channels,
         double(pcm->frames()) / std::max(1, pcm->rate));
    std::lock_guard guard(lock);
    cache[hostPath] = pcm;
    return pcm;
}
} // namespace audio
