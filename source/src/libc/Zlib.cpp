// libz: uncompress, inflate (zlib / gzip / raw streams) and gzopen/gzread for reading. The whole input given to a
// z_stream is decoded at once into a host buffer and handed out as the app's output space allows.
// z_stream (32-bit): next_in +0, avail_in +4, total_in +8, next_out +12, avail_out +16, total_out +20, msg +24,
// state +28, zalloc +32, zfree +36, opaque +40, data_type +44, adler +48, reserved +52.
#include "Hle.h"
#include "Log.h"
#include "Memory.h"
#include "libc/Files.h"
#include <cstring>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

using hle::arg;
using hle::ret;

namespace zlib {
namespace {
// ---------------------------------------------------------------- RFC 1951 inflate
struct Bits {
    const std::uint8_t *data;
    std::size_t size, pos = 0;
    std::uint32_t buffer = 0;
    int count = 0;
    bool overrun = false;
    int bit() {
        if (!count) {
            if (pos >= size) { overrun = true; return 0; }
            buffer = data[pos++];
            count = 8;
        }
        const int b = buffer & 1;
        buffer >>= 1;
        --count;
        return b;
    }
    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; ++i) v |= bit() << i;
        return v;
    }
    void align() { count = 0; }
};
struct Huffman {
    std::uint16_t counts[16]{}, symbols[288]{};
    void build(const std::uint8_t *lengths, int n) {
        std::memset(counts, 0, sizeof counts);
        for (int i = 0; i < n; ++i) ++counts[lengths[i]];
        counts[0] = 0;
        std::uint16_t offsets[16]{};
        for (int i = 1; i < 16; ++i) offsets[i] = std::uint16_t(offsets[i - 1] + counts[i - 1]);
        for (int i = 0; i < n; ++i) if (lengths[i]) symbols[offsets[lengths[i]]++] = std::uint16_t(i);
    }
    int decode(Bits &in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= in.bit();
            const int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (in.overrun) return -1;
        }
        return -1;
    }
};
const std::uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                     4097, 6145, 8193, 12289, 16385, 24577};
const std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
bool block(Bits &in, std::vector<std::uint8_t> &out, const Huffman &lit, const Huffman &dist) {
    for (;;) {
        const int sym = lit.decode(in);
        if (sym < 0) return false;
        if (sym < 256) { out.push_back(std::uint8_t(sym)); continue; }
        if (sym == 256) return true;
        const int li = sym - 257;
        if (li >= 29) return false;
        const int length = kLengthBase[li] + in.bits(kLengthExtra[li]);
        const int di = dist.decode(in);
        if (di < 0 || di >= 30) return false;
        const std::size_t distance = kDistBase[di] + std::size_t(in.bits(kDistExtra[di]));
        if (distance > out.size()) return false;
        const std::size_t from = out.size() - distance;
        for (int i = 0; i < length; ++i) out.push_back(out[from + std::size_t(i)]);
        if (in.overrun) return false;
    }
}
// Raw deflate data; returns the bytes consumed (0 on error).
std::size_t inflateRaw(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &out) {
    Bits in{data, size};
    for (;;) {
        const int last = in.bit();
        const int type = in.bits(2);
        if (type == 0) {
            in.align();
            if (in.pos + 4 > size) return 0;
            const std::size_t len = data[in.pos] | (data[in.pos + 1] << 8);
            in.pos += 4;
            if (in.pos + len > size) return 0;
            out.insert(out.end(), data + in.pos, data + in.pos + len);
            in.pos += len;
        } else if (type == 1) {
            static Huffman lit, dist;
            static bool built = [] {
                std::uint8_t l[288];
                for (int i = 0; i < 144; ++i) l[i] = 8;
                for (int i = 144; i < 256; ++i) l[i] = 9;
                for (int i = 256; i < 280; ++i) l[i] = 7;
                for (int i = 280; i < 288; ++i) l[i] = 8;
                lit.build(l, 288);
                std::uint8_t d[30];
                for (auto &x : d) x = 5;
                dist.build(d, 30);
                return true;
            }();
            (void)built;
            if (!block(in, out, lit, dist)) return 0;
        } else if (type == 2) {
            const int hlit = in.bits(5) + 257, hdist = in.bits(5) + 1, hclen = in.bits(4) + 4;
            static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t codeLengths[19]{};
            for (int i = 0; i < hclen; ++i) codeLengths[order[i]] = std::uint8_t(in.bits(3));
            Huffman lengthCode;
            lengthCode.build(codeLengths, 19);
            std::uint8_t lengths[320]{};
            for (int i = 0; i < hlit + hdist;) {
                const int sym = lengthCode.decode(in);
                if (sym < 0) return 0;
                if (sym < 16) lengths[i++] = std::uint8_t(sym);
                else {
                    int repeat = 0;
                    std::uint8_t value = 0;
                    if (sym == 16) { if (!i) return 0; value = lengths[i - 1]; repeat = 3 + in.bits(2); }
                    else if (sym == 17) repeat = 3 + in.bits(3);
                    else repeat = 11 + in.bits(7);
                    while (repeat-- && i < hlit + hdist) lengths[i++] = value;
                }
            }
            Huffman lit, dist;
            lit.build(lengths, hlit);
            dist.build(lengths + hlit, hdist);
            if (!block(in, out, lit, dist)) return 0;
        } else return 0;
        if (in.overrun) return 0;
        if (last) return in.pos;
    }
}
} // namespace
// zlib (RFC 1950), gzip (RFC 1952) or raw deflate, detected from the header (windowBits semantics: > 15 + 16 gzip,
// + 32 auto, negative raw). Returns false on corrupt data.
bool decompress(const std::uint8_t *data, std::size_t size, std::vector<std::uint8_t> &out, int windowBits = 47) {
    if (windowBits < 0) return inflateRaw(data, size, out) != 0;
    if (size >= 10 && data[0] == 0x1F && data[1] == 0x8B) { // gzip
        const int flags = data[3];
        std::size_t at = 10;
        if (flags & 4) { if (at + 2 > size) return false; at += 2 + (data[at] | (data[at + 1] << 8)); }
        if (flags & 8) { while (at < size && data[at]) ++at; ++at; }
        if (flags & 16) { while (at < size && data[at]) ++at; ++at; }
        if (flags & 2) at += 2;
        return at < size && inflateRaw(data + at, size - at, out) != 0;
    }
    if (size >= 2 && (data[0] & 0x0F) == 8 && ((data[0] << 8) | data[1]) % 31 == 0) return inflateRaw(data + 2, size - 2, out) != 0;
    return inflateRaw(data, size, out) != 0;
}
} // namespace zlib

// uncompress(dest, destLen *, source, sourceLen): Z_OK 0, Z_BUF_ERROR -5, Z_DATA_ERROR -3.
HLE(uncompress) {
    std::vector<std::uint8_t> out;
    if (!zlib::decompress(mem::base() + arg(2), std::size_t(arg(3)), out, 15)) return ret(std::uint32_t(-3));
    const auto capacity = mem::r32(arg(1));
    if (out.size() > capacity) return ret(std::uint32_t(-5));
    std::memcpy(mem::base() + arg(0), out.data(), out.size());
    mem::w32(arg(1), std::uint32_t(out.size()));
    ret(0);
}

namespace {
struct Stream {
    std::vector<std::uint8_t> input, output;
    std::size_t delivered = 0;
    int windowBits = 15;
    bool decoded = false, failed = false;
};
std::mutex g_streamsLock;
std::map<std::uint64_t, Stream> g_streams; // keyed by z_stream address
} // namespace
// inflateInit2_(stream, windowBits, version, size) / inflateInit_(stream, version, size)
HLE(inflateInit2_) {
    std::lock_guard lock(g_streamsLock);
    g_streams[arg(0)] = Stream{};
    g_streams[arg(0)].windowBits = std::int32_t(arg(1));
    mem::w32(arg(0) + 8, 0);
    mem::w32(arg(0) + 20, 0);
    ret(0);
}
HLE(inflateInit_) {
    std::lock_guard lock(g_streamsLock);
    g_streams[arg(0)] = Stream{};
    mem::w32(arg(0) + 8, 0);
    mem::w32(arg(0) + 20, 0);
    ret(0);
}
// inflate(stream, flush): all available input is consumed and decoded at once; output is handed out in pieces.
// Z_OK 0, Z_STREAM_END 1, Z_BUF_ERROR -5, Z_DATA_ERROR -3.
HLE(inflate) {
    std::lock_guard lock(g_streamsLock);
    auto found = g_streams.find(arg(0));
    if (found == g_streams.end()) return ret(std::uint32_t(-2));
    auto &s = found->second;
    const auto z = arg(0);
    const auto availIn = mem::r32(z + 4);
    if (availIn) {
        const auto next = mem::rp(z);
        s.input.insert(s.input.end(), mem::base() + next, mem::base() + next + availIn);
        mem::wp(z, next + availIn);
        mem::w32(z + 4, 0);
        mem::w32(z + 8, mem::r32(z + 8) + availIn);
    }
    if (!s.decoded) {
        s.output.clear();
        if (zlib::decompress(s.input.data(), s.input.size(), s.output, s.windowBits)) s.decoded = true;
        else if (std::uint32_t(arg(1)) == 4) { s.failed = true; return ret(std::uint32_t(-3)); } // Z_FINISH with bad data
        else return ret(availIn ? 0 : std::uint32_t(-5));                                       // need more input
    }
    const auto availOut = mem::r32(z + 16);
    const auto n = std::min<std::size_t>(availOut, s.output.size() - s.delivered);
    const auto out = mem::rp(z + 12);
    std::memcpy(mem::base() + out, s.output.data() + s.delivered, n);
    s.delivered += n;
    mem::wp(z + 12, out + n);
    mem::w32(z + 16, std::uint32_t(availOut - n));
    mem::w32(z + 20, mem::r32(z + 20) + std::uint32_t(n));
    ret(s.delivered == s.output.size() ? 1 : 0);
}
HLE(inflateEnd) {
    std::lock_guard lock(g_streamsLock);
    g_streams.erase(arg(0));
    ret(0);
}
HLE(inflateReset) {
    std::lock_guard lock(g_streamsLock);
    const int bits = g_streams[arg(0)].windowBits;
    g_streams[arg(0)] = Stream{};
    g_streams[arg(0)].windowBits = bits;
    ret(0);
}

// gzopen(path, mode) for reading: the file is decompressed up front.
namespace {
struct GzFile { std::vector<std::uint8_t> data; std::size_t pos = 0; };
std::map<std::uint64_t, GzFile> g_gzFiles;
std::uint64_t g_nextGz = 0x6000;
} // namespace
HLE(gzopen) {
    const std::string path = mem::cstring(arg(0)), mode = mem::cstring(arg(1));
    if (mode.find('r') == std::string::npos) { hlog("zlib", "gzopen %s for writing is not supported", path.c_str()); return ret(0); }
    std::ifstream in(std::filesystem::u8path(files::hostPath(path)), std::ios::binary);
    if (!in) return ret(0);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    GzFile f;
    if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B) {
        if (!zlib::decompress(bytes.data(), bytes.size(), f.data)) return ret(0);
    } else f.data = bytes; // gzread also reads uncompressed files
    std::lock_guard lock(g_streamsLock);
    const auto handle = g_nextGz++;
    g_gzFiles[handle] = std::move(f);
    ret(handle);
}
HLE(gzread) {
    std::lock_guard lock(g_streamsLock);
    auto found = g_gzFiles.find(arg(0));
    if (found == g_gzFiles.end()) return ret(std::uint32_t(-1));
    auto &f = found->second;
    const auto n = std::min<std::size_t>(std::size_t(arg(2)), f.data.size() - f.pos);
    std::memcpy(mem::base() + arg(1), f.data.data() + f.pos, n);
    f.pos += n;
    ret(std::uint32_t(n));
}
HLE(gzclose) {
    std::lock_guard lock(g_streamsLock);
    g_gzFiles.erase(arg(0));
    ret(0);
}
