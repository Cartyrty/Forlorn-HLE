#pragma once
// HLE function and data registry. Handlers read their arguments as 32-bit words (iOS armv7 ABI: r0-r3, then the
// stack; floats are passed as their bits (softfp); 64-bit values take two consecutive words) and return in r0 (r1).
#include "Cpu.h"
#include <cstdint>
#include <functional>
#include <string>

namespace hle {
using Handler = void (*)();
// Mach-O symbol names, with the leading underscore (e.g. "_malloc").
void registerFunction(const std::string &name, Handler handler);
// A data import: the factory returns the guest address of the variable, created on first use.
void registerData(const std::string &name, std::function<std::uint64_t()> factory);
// Data imports by name prefix (e.g. "_OBJC_CLASS_$_"): the factory receives the full name.
void registerDataPrefix(const std::string &prefix, std::function<std::uint64_t(const std::string &)> factory);
// The address an import binds to (a trap stub for functions, the variable for data).
std::uint64_t resolveImport(const std::string &name);
// A trap stub calling a host handler; used for host-implemented IMPs and callbacks handed to the guest.
std::uint64_t makeStub(const std::string &name, Handler handler);
void dispatch(std::uint32_t swi);
// Replaces a function in the app image with a host handler (its entry jumps to an "SVC #n; BX LR" stub). The
// symbol may be an Objective-C method name ("-[Class selector]"); patchAddress takes the address (bit 0 = Thumb).
bool patchFunction(const std::string &symbol, Handler handler);
bool patchAddress(std::uint64_t address, const std::string &name, Handler handler);
// Runs a host handler on entry to an app function, then the original function (not supported on armv7).
bool hookFunction(const std::string &symbol, Handler before);
// Patches registered at static-init time, applied after the image is loaded.
void applyPatches();
// Work to run once the image is loaded (e.g. hooks enabled by environment variables).
void afterLoad(std::function<void()> fn);
struct PatchRegistrar {
    PatchRegistrar(const char *symbol, Handler handler);
};
bool looksLikeData(const std::string &name);
void createSpecialStubs();
const std::string &currentFunction();

struct Registrar {
    Registrar(const char *name, Handler handler) { registerFunction(name, handler); }
};
struct DataRegistrar {
    DataRegistrar(const char *name, std::function<std::uint64_t()> factory) { registerData(name, std::move(factory)); }
};

// The i-th argument word: r0-r3, then the caller's stack (variadic arguments simply continue the sequence).
std::uint64_t arg(int i);
inline void ret(std::uint64_t value) { cpu::setX(0, value); }
// 64-bit results in r0 (low) and r1 (high).
inline void ret64(std::uint64_t value) { cpu::setX(0, value & 0xFFFFFFFF); cpu::setX(1, value >> 32); }
inline void retFloat(float value) { cpu::setX(0, cpu::floatBits(value)); }
void retDouble(double value);
float argFloat(int i);                 // a float argument word
double argDouble(int i);               // words i and i + 1
std::int64_t arg64(int i);             // words i (low) and i + 1 (high)
inline std::int32_t argInt(int i) { return std::int32_t(arg(i)); }
inline std::uint64_t varg(int i) { return arg(i); }
// Debug: logs when the word at `address` changes (checked at every HLE call).
void watch(std::uint32_t address);
} // namespace hle

#define HLE_CAT2(a, b) a##b
#define HLE_CAT(a, b) HLE_CAT2(a, b)
// HLE(malloc) { ... } defines the handler for the C symbol _malloc.
#define HLE(name)                                                                                                      \
    static void HLE_CAT(hle_fn_, name)();                                                                              \
    static ::hle::Registrar HLE_CAT(hle_reg_, name)("_" #name, HLE_CAT(hle_fn_, name));                               \
    static void HLE_CAT(hle_fn_, name)()
// HLE_NAMED("_select$DARWIN_EXTSN", select_darwin) { ... } for symbol names that are not identifiers.
#define HLE_NAMED(symbol, id)                                                                                          \
    static void HLE_CAT(hle_fn_, id)();                                                                                \
    static ::hle::Registrar HLE_CAT(hle_reg_, id)(symbol, HLE_CAT(hle_fn_, id));                                      \
    static void HLE_CAT(hle_fn_, id)()
