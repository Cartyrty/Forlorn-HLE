#pragma once
// printf-family formatting over guest arguments (iOS armv7): variadic arguments are consecutive 32-bit words, in r0-r3
// and then on the stack for a variadic call, or in memory for a va_list (a plain pointer). 64-bit values (long long,
// double) take two words with no alignment padding.
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace libc {
struct ArgSource {
    std::uint64_t pointer = 0; // va_list: the next word in memory
    int index = -1;            // >= 0: the next HLE argument word (registers, then the caller's stack)
    static ArgSource fromPointer(std::uint64_t p) { ArgSource a; a.pointer = p; return a; }
    static ArgSource fromArgs(int firstIndex) { ArgSource a; a.index = firstIndex; return a; }
    std::uint32_t next();
    std::uint64_t next64() {
        const std::uint64_t low = next();
        return low | (std::uint64_t(next()) << 32);
    }
    double nextDouble() {
        const auto bits = next64();
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
};
// objectFormatter handles %@ (an Objective-C object's description); nullptr prints the pointer.
std::string format(const std::string &fmt, ArgSource args, const std::function<std::string(std::uint64_t)> &objectFormatter = nullptr);
} // namespace libc
