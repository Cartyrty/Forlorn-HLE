// libSystem: memory, strings, ctype, stdlib, math.
#include "Hle.h"
#include "Log.h"
#include "Memory.h"
#include "libc/Format.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <random>

using hle::arg;
using hle::ret;
namespace {
std::uint8_t *P(std::uint64_t a) { return mem::base() + a; }
char *C(std::uint64_t a) { return reinterpret_cast<char *>(mem::base() + a); }
// softfp: a double argument is two words, a float one word.
double D(int word) { return hle::argDouble(word); }
float F(int word) { return hle::argFloat(word); }
} // namespace

// --- Memory ---
HLE(malloc) { ret(mem::alloc(std::size_t(arg(0)))); }
HLE(calloc) { ret(mem::calloc(std::size_t(arg(0) * arg(1)))); }
HLE(realloc) { ret(mem::realloc(arg(0), std::size_t(arg(1)))); }
HLE(free) { mem::free(arg(0)); }
HLE(malloc_size) { ret(mem::allocSize(arg(0))); }
HLE(valloc) { ret(mem::alloc(std::size_t(arg(0)))); }
HLE(posix_memalign) {
    // 16-byte alignment is all the heap gives; larger alignments are rare here.
    mem::wp(arg(0), mem::alloc(std::size_t(arg(2))));
    ret(0);
}
HLE(memcpy) { std::memmove(P(arg(0)), P(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(memmove) { std::memmove(P(arg(0)), P(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(__memcpy_chk) { std::memmove(P(arg(0)), P(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(__memmove_chk) { std::memmove(P(arg(0)), P(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(memset) { std::memset(P(arg(0)), int(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(__memset_chk) { std::memset(P(arg(0)), int(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(bzero) { std::memset(P(arg(0)), 0, std::size_t(arg(1))); }
HLE(memcmp) { ret(std::uint64_t(std::int64_t(std::memcmp(P(arg(0)), P(arg(1)), std::size_t(arg(2)))))); }
HLE(memchr) {
    auto *found = std::memchr(P(arg(0)), int(arg(1)), std::size_t(arg(2)));
    ret(found ? mem::guestAddress(found) : 0);
}
HLE(memset_pattern16) {
    for (std::uint64_t i = 0; i < arg(2); ++i) *P(arg(0) + i) = *P(arg(1) + (i & 15));
}

// --- Strings ---
HLE(strlen) { ret(std::strlen(C(arg(0)))); }
HLE(strnlen) { ret(strnlen(C(arg(0)), std::size_t(arg(1)))); }
HLE(strcmp) { ret(std::uint64_t(std::int64_t(std::strcmp(C(arg(0)), C(arg(1)))))); }
HLE(strncmp) { ret(std::uint64_t(std::int64_t(std::strncmp(C(arg(0)), C(arg(1)), std::size_t(arg(2)))))); }
// Case-insensitive comparisons (ASCII, as in the C locale).
static int compareNoCase(const char *a, const char *b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const int ca = std::tolower(static_cast<unsigned char>(a[i])), cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}
HLE(strcasecmp) { ret(std::uint64_t(std::int64_t(compareNoCase(C(arg(0)), C(arg(1)), ~std::size_t(0))))); }
HLE(strncasecmp) { ret(std::uint64_t(std::int64_t(compareNoCase(C(arg(0)), C(arg(1)), std::size_t(arg(2)))))); }
HLE(strcpy) { std::strcpy(C(arg(0)), C(arg(1))); ret(arg(0)); }
HLE(__strcpy_chk) { std::strcpy(C(arg(0)), C(arg(1))); ret(arg(0)); }
HLE(strncpy) { std::strncpy(C(arg(0)), C(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(__strncpy_chk) { std::strncpy(C(arg(0)), C(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(strcat) { std::strcat(C(arg(0)), C(arg(1))); ret(arg(0)); }
HLE(__strcat_chk) { std::strcat(C(arg(0)), C(arg(1))); ret(arg(0)); }
HLE(strncat) { std::strncat(C(arg(0)), C(arg(1)), std::size_t(arg(2))); ret(arg(0)); }
HLE(strlcpy) {
    const auto len = std::strlen(C(arg(1)));
    if (arg(2)) {
        const auto n = std::min<std::size_t>(len, std::size_t(arg(2)) - 1);
        std::memcpy(C(arg(0)), C(arg(1)), n);
        C(arg(0))[n] = 0;
    }
    ret(len);
}
HLE(strlcat) {
    const auto dl = strnlen(C(arg(0)), std::size_t(arg(2))), sl = std::strlen(C(arg(1)));
    if (dl < arg(2)) {
        const auto n = std::min<std::size_t>(sl, std::size_t(arg(2)) - dl - 1);
        std::memcpy(C(arg(0)) + dl, C(arg(1)), n);
        C(arg(0))[dl + n] = 0;
    }
    ret(dl + sl);
}
HLE(strdup) { ret(mem::heapString(C(arg(0)))); }
HLE(strndup) { ret(mem::heapString(std::string(C(arg(0)), strnlen(C(arg(0)), std::size_t(arg(1)))))); }
HLE(strchr) { auto *f = std::strchr(C(arg(0)), int(arg(1))); ret(f ? mem::guestAddress(f) : 0); }
HLE(strrchr) { auto *f = std::strrchr(C(arg(0)), int(arg(1))); ret(f ? mem::guestAddress(f) : 0); }
HLE(strstr) { auto *f = std::strstr(C(arg(0)), C(arg(1))); ret(f ? mem::guestAddress(f) : 0); }
HLE(strcasestr) {
    std::string h = C(arg(0)), n = C(arg(1));
    for (auto &c : h) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (auto &c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto pos = h.find(n);
    ret(pos == std::string::npos ? 0 : arg(0) + pos);
}
HLE(strpbrk) { auto *f = std::strpbrk(C(arg(0)), C(arg(1))); ret(f ? mem::guestAddress(f) : 0); }
HLE(strspn) { ret(std::strspn(C(arg(0)), C(arg(1)))); }
HLE(strcspn) { ret(std::strcspn(C(arg(0)), C(arg(1)))); }
HLE(strsep) {
    const auto sp = mem::rp(arg(0));
    if (!sp) return ret(0);
    char *s = C(sp);
    char *end = std::strpbrk(s, C(arg(1)));
    if (end) { *end = 0; mem::wp(arg(0), mem::guestAddress(end + 1)); }
    else mem::wp(arg(0), 0);
    ret(sp);
}
HLE(strtok_r) {
    std::uint64_t s = arg(0);
    if (!s) s = mem::rp(arg(2));
    if (!s) return ret(0);
    char *p = C(s);
    p += std::strspn(p, C(arg(1)));
    if (!*p) { mem::wp(arg(2), 0); return ret(0); }
    char *end = p + std::strcspn(p, C(arg(1)));
    if (*end) { *end = 0; mem::wp(arg(2), mem::guestAddress(end + 1)); }
    else mem::wp(arg(2), 0);
    ret(mem::guestAddress(p));
}
HLE(strtok) {
    static thread_local std::uint64_t save = 0;
    std::uint64_t s = arg(0) ? arg(0) : save;
    if (!s) return ret(0);
    char *p = C(s);
    p += std::strspn(p, C(arg(1)));
    if (!*p) { save = 0; return ret(0); }
    char *end = p + std::strcspn(p, C(arg(1)));
    if (*end) { *end = 0; save = mem::guestAddress(end + 1); }
    else save = 0;
    ret(mem::guestAddress(p));
}
HLE(strerror) {
    static std::uint64_t buffer = 0;
    if (!buffer) buffer = mem::hleAlloc(128);
    std::snprintf(C(buffer), 128, "error %d", int(arg(0)));
    ret(buffer);
}
// long is 32 bits on armv7; long long results come back in r0:r1.
HLE(strtol) {
    char *end;
    const long v = std::strtol(C(arg(0)), &end, int(arg(2)));
    if (arg(1)) mem::wp(arg(1), mem::guestAddress(end));
    ret(std::uint64_t(std::uint32_t(v)));
}
HLE(strtoll) {
    char *end;
    const long long v = std::strtoll(C(arg(0)), &end, int(arg(2)));
    if (arg(1)) mem::wp(arg(1), mem::guestAddress(end));
    hle::ret64(std::uint64_t(v));
}
HLE(strtoul) {
    char *end;
    const unsigned long v = std::strtoul(C(arg(0)), &end, int(arg(2)));
    if (arg(1)) mem::wp(arg(1), mem::guestAddress(end));
    ret(v);
}
HLE(strtoull) {
    char *end;
    const unsigned long long v = std::strtoull(C(arg(0)), &end, int(arg(2)));
    if (arg(1)) mem::wp(arg(1), mem::guestAddress(end));
    hle::ret64(v);
}
HLE(strtod) {
    char *end;
    const double v = std::strtod(C(arg(0)), &end);
    if (arg(1)) mem::wp(arg(1), mem::guestAddress(end));
    hle::retDouble(v);
}
HLE(strtof) {
    char *end;
    const float v = std::strtof(C(arg(0)), &end);
    if (arg(1)) mem::wp(arg(1), mem::guestAddress(end));
    hle::retFloat(v);
}
HLE(atoi) { ret(std::uint64_t(std::int64_t(std::atoi(C(arg(0)))))); }
HLE(atol) { ret(std::uint32_t(std::atol(C(arg(0))))); }
HLE(atoll) { hle::ret64(std::uint64_t(std::atoll(C(arg(0))))); }
HLE(atof) { hle::retDouble(std::atof(C(arg(0)))); }

// --- ctype ---
// __maskrune(c, mask): Apple's rune table classes (_CTYPE_A 0x100, C 0x200, D 0x400, G 0x800, L 0x1000, P 0x2000,
// S 0x4000, U 0x8000, X 0x10000, B 0x20000, R 0x40000).
static std::uint32_t runeType(int c) {
    if (c < 0 || c > 127) return 0;
    std::uint32_t t = 0;
    if (std::isalpha(c)) t |= 0x100;
    if (std::iscntrl(c)) t |= 0x200;
    if (std::isdigit(c)) t |= 0x400 | 0x10000;
    if (std::isgraph(c)) t |= 0x800;
    if (std::islower(c)) t |= 0x1000;
    if (std::ispunct(c)) t |= 0x2000;
    if (std::isspace(c)) t |= 0x4000;
    if (std::isupper(c)) t |= 0x8000;
    if (std::isxdigit(c)) t |= 0x10000;
    if (c == ' ' || c == '\t') t |= 0x20000;
    if (std::isprint(c)) t |= 0x40000;
    return t;
}
HLE(__maskrune) { ret(runeType(int(arg(0))) & std::uint32_t(arg(1))); }
HLE(__tolower) { ret(std::uint64_t(std::tolower(int(arg(0))))); }
HLE(__toupper) { ret(std::uint64_t(std::toupper(int(arg(0))))); }
HLE(tolower) { ret(std::uint64_t(std::tolower(int(arg(0))))); }
HLE(toupper) { ret(std::uint64_t(std::toupper(int(arg(0))))); }
HLE(isalpha) { ret(std::isalpha(int(arg(0))) != 0); }
HLE(isdigit) { ret(std::isdigit(int(arg(0))) != 0); }
HLE(isspace) { ret(std::isspace(int(arg(0))) != 0); }
HLE(isalnum) { ret(std::isalnum(int(arg(0))) != 0); }
// _DefaultRuneLocale: struct _RuneLocale { char magic[8]; char encoding[32]; 2 function pointers; int invalid_rune;
// __uint32_t runetype[256]... }. Only runetype/maplower/mapupper are read by inline ctype macros.
static hle::DataRegistrar d_runeLocale("__DefaultRuneLocale", [] {
    const auto locale = mem::hleAlloc(0x1000, 16);
    std::memcpy(P(locale), "RuneMagA", 8);
    std::memcpy(P(locale + 8), "NONE", 4);
    // 32-bit: magic[8] +0, encoding[32] +8, sgetrune +40, sputrune +44, invalid_rune +48, runetype[256] +0x34,
    // maplower[256] +0x434, mapupper[256] +0x834.
    for (int c = 0; c < 256; ++c) {
        mem::w32(locale + 0x34 + 4 * c, runeType(c));
        mem::w32(locale + 0x434 + 4 * c, std::uint32_t(c < 128 ? std::tolower(c) : c));
        mem::w32(locale + 0x834 + 4 * c, std::uint32_t(c < 128 ? std::toupper(c) : c));
    }
    return locale;
});

// --- stdlib ---
namespace {
std::mt19937 g_rng(12345);
std::uint64_t g_rand48 = 0x1234ABCD330E;
} // namespace
HLE(rand) { ret(std::uint64_t(g_rng() & 0x7FFFFFFF)); }
HLE(srand) { g_rng.seed(std::uint32_t(arg(0))); }
HLE(random) { ret(std::uint64_t(g_rng() & 0x7FFFFFFF)); }
HLE(srandom) { g_rng.seed(std::uint32_t(arg(0))); }
HLE(arc4random) { ret(std::uint64_t(std::uint32_t(g_rng()))); }
HLE(arc4random_uniform) { ret(arg(0) ? std::uint64_t(g_rng() % std::uint32_t(arg(0))) : 0); }
HLE(arc4random_buf) { for (std::uint64_t i = 0; i < arg(1); ++i) *P(arg(0) + i) = std::uint8_t(g_rng()); }
HLE(srand48) { g_rand48 = (std::uint64_t(std::uint32_t(arg(0))) << 16) | 0x330E; }
HLE(drand48) {
    g_rand48 = (g_rand48 * 0x5DEECE66Dull + 0xB) & ((1ull << 48) - 1);
    hle::retDouble(double(g_rand48) / double(1ull << 48));
}
HLE(abs) { ret(std::uint64_t(std::int64_t(std::abs(std::int32_t(arg(0)))))); }
HLE(labs) { ret(std::uint32_t(std::abs(std::int32_t(arg(0))))); }
HLE(qsort) {
    // qsort(base, n, size, compare): a simple insertion-merge via std::stable_sort on indices, calling the guest
    // comparator for each comparison.
    const auto base = arg(0), n = arg(1), size = arg(2), compare = arg(3);
    if (n < 2) return;
    std::vector<std::vector<std::uint8_t>> items{std::size_t(n)};
    for (std::uint64_t i = 0; i < n; ++i) items[std::size_t(i)].assign(P(base + i * size), P(base + (i + 1) * size));
    const auto scratchA = mem::alloc(std::size_t(size)), scratchB = mem::alloc(std::size_t(size));
    std::stable_sort(items.begin(), items.end(), [&](const auto &a, const auto &b) {
        std::memcpy(P(scratchA), a.data(), std::size_t(size));
        std::memcpy(P(scratchB), b.data(), std::size_t(size));
        return std::int32_t(cpu::call(compare, {scratchA, scratchB})) < 0;
    });
    for (std::uint64_t i = 0; i < n; ++i) std::memcpy(P(base + i * size), items[std::size_t(i)].data(), std::size_t(size));
    mem::free(scratchA);
    mem::free(scratchB);
}
HLE(getenv) { ret(0); }
HLE(setenv) { ret(0); }
HLE(abort) { cpu::backtrace("abort()"); fatal("guest called abort()"); }
HLE(exit) { hlog("process", "exit(%d)", int(arg(0))); std::exit(int(arg(0))); }
HLE(_exit) { std::exit(int(arg(0))); }
HLE(__assert_rtn) {
    cpu::backtrace("assertion");
    fatal("assertion failed: %s (%s:%d in %s)", C(arg(3)), C(arg(1)), int(arg(2)), C(arg(0)));
}
HLE(__stack_chk_fail) { cpu::backtrace("stack protector"); fatal("__stack_chk_fail"); }
static hle::DataRegistrar d_stackGuard("___stack_chk_guard", [] {
    const auto a = mem::hleAlloc(4);
    mem::wp(a, 0x5A5AA5A5u);
    return a;
});
HLE(system) { ret(0); } // system(NULL) = 0: no shell (stock device)
HLE(backtrace) { ret(0); }
HLE(backtrace_symbols_fd) {}
HLE(raise) { warn("raise(%d)", int(arg(0))); ret(0); }
HLE(signal) { ret(0); }
HLE(sigaction) { ret(0); }
HLE(sigaltstack) { ret(0); }
HLE(setjmp) {
    // setjmp/longjmp are used by libpng-style error paths; a setjmp that is never jumped to is enough.
    ret(0);
}
HLE(longjmp) { cpu::backtrace("longjmp"); fatal("longjmp is not supported"); }

// --- math (softfp: doubles in r0:r1 / r2:r3, floats in r0 / r1; results in r0(:r1)) ---
#define MATH1(name) HLE(name) { hle::retDouble(std::name(D(0))); }
#define MATH1F(name, fn) HLE(name) { hle::retFloat(std::fn(F(0))); }
#define MATH2(name) HLE(name) { hle::retDouble(std::name(D(0), D(2))); }
#define MATH2F(name, fn) HLE(name) { hle::retFloat(std::fn(F(0), F(1))); }
MATH1(sin) MATH1(cos) MATH1(tan) MATH1(asin) MATH1(acos) MATH1(atan) MATH1(exp) MATH1(exp2) MATH1(log) MATH1(log10)
MATH1(log2) MATH1(sqrt) MATH1(floor) MATH1(ceil) MATH1(round) MATH1(trunc) MATH1(fabs) MATH1(sinh) MATH1(cosh)
MATH1(tanh)
MATH2(pow) MATH2(atan2) MATH2(fmod) MATH2(fmax) MATH2(fmin) MATH2(hypot)
MATH1F(sinf, sin) MATH1F(cosf, cos) MATH1F(tanf, tan) MATH1F(asinf, asin) MATH1F(acosf, acos) MATH1F(atanf, atan)
MATH1F(expf, exp) MATH1F(logf, log) MATH1F(log10f, log10) MATH1F(sqrtf, sqrt) MATH1F(floorf, floor) MATH1F(ceilf, ceil)
MATH1F(roundf, round) MATH1F(fabsf, fabs) MATH1F(truncf, trunc) MATH1F(exp2f, exp2) MATH1F(log2f, log2)
MATH2F(powf, pow) MATH2F(atan2f, atan2) MATH2F(fmodf, fmod) MATH2F(fmaxf, fmax) MATH2F(fminf, fmin) MATH2F(hypotf, hypot)
HLE(lroundf) { ret(std::uint32_t(std::lround(F(0)))); }
HLE(lround) { ret(std::uint32_t(std::lround(D(0)))); }
HLE(lrint) { ret(std::uint32_t(std::lrint(D(0)))); }
HLE(lrintf) { ret(std::uint32_t(std::lrint(F(0)))); }
HLE(ldexp) { hle::retDouble(std::ldexp(D(0), int(arg(2)))); }
HLE(ldexpf) { hle::retFloat(std::ldexp(F(0), int(arg(1)))); }
HLE(frexp) { int e; const double r = std::frexp(D(0), &e); mem::w32(arg(2), std::uint32_t(e)); hle::retDouble(r); }
HLE(modf) { double i; const double r = std::modf(D(0), &i); mem::write(arg(2), i); hle::retDouble(r); }

// --- compiler-rt helpers exported by libSystem (no hardware divide on armv7 cores before Swift) ---
HLE(__divsi3) { const auto b = std::int32_t(arg(1)); ret(std::uint32_t(b ? std::int32_t(arg(0)) / b : 0)); }
HLE(__modsi3) { const auto b = std::int32_t(arg(1)); ret(std::uint32_t(b ? std::int32_t(arg(0)) % b : 0)); }
HLE(__udivsi3) { const auto b = std::uint32_t(arg(1)); ret(b ? std::uint32_t(arg(0)) / b : 0); }
HLE(__umodsi3) { const auto b = std::uint32_t(arg(1)); ret(b ? std::uint32_t(arg(0)) % b : 0); }
HLE(__udivdi3) { const auto b = std::uint64_t(hle::arg64(2)); hle::ret64(b ? std::uint64_t(hle::arg64(0)) / b : 0); }
HLE(__umoddi3) { const auto b = std::uint64_t(hle::arg64(2)); hle::ret64(b ? std::uint64_t(hle::arg64(0)) % b : 0); }
HLE(__divdi3) { const auto b = hle::arg64(2); hle::ret64(std::uint64_t(b ? hle::arg64(0) / b : 0)); }
HLE(__moddi3) { const auto b = hle::arg64(2); hle::ret64(std::uint64_t(b ? hle::arg64(0) % b : 0)); }
