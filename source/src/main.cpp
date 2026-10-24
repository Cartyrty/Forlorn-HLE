#include "Cpu.h"
#include "Hle.h"
#include "Loader.h"
#include "Log.h"
#include "Memory.h"
#include "Runtime.h"
#include "libc/Files.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

// ForlornHLE [Forlorn.app directory]
int main(int argc, char **argv) {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no modal abort() dialog; the log has the cause
#endif
    namespace fs = std::filesystem;
    const fs::path exeDir = fs::absolute(fs::path(argv[0]).parent_path());
    fs::path app = argc > 1 ? fs::path(argv[1]) : fs::path();
    if (app.empty()) {
        // Default: Forlorn.app next to the exe or in a parent folder (the project's inputs/ipa/Payload copy).
        for (fs::path dir = exeDir; !dir.empty() && dir != dir.root_path(); dir = dir.parent_path()) {
            if (fs::exists(dir / "Forlorn.app/Forlorn")) { app = dir / "Forlorn.app"; break; }
            if (fs::exists(dir / "inputs/ipa/Payload/Forlorn.app/Forlorn")) { app = dir / "inputs/ipa/Payload/Forlorn.app"; break; }
        }
    }
    if (app.empty() || !fs::exists(app / "Forlorn")) fatal("usage: ForlornHLE <Forlorn.app directory>");
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(); };

    mem::init();
    cpu::initProcess();
    hle::createSpecialStubs();
    runtime::setBundlePath(app.string());
    // HLE_SANDBOX: another app container (its own Documents and settings).
    const char *sandbox = std::getenv("HLE_SANDBOX");
    files::setSandboxRoot(sandbox ? std::string(sandbox) : (exeDir / "sandbox").string());
    loader::load((app / "Forlorn").string());
    hlog("startup", "Mach-O loading and binding elapsed_ms=%lld", elapsed());

    const auto stackTop = mem::allocStack(8 * 1024 * 1024);
    cpu::attachThread(stackTop);
    // HLE_SELFTEST=1: checks a few VFP/NEON instructions the game relies on, then exits.
    if (std::getenv("HLE_SELFTEST")) {
        auto run = [](std::initializer_list<std::uint32_t> code, float a, float b) {
            const auto at = mem::hleAlloc(64, 8);
            std::uint64_t p = at;
            for (auto w : code) { mem::w32(p, w); p += 4; }
            mem::w32(p, 0xE12FFF1E); // bx lr
            cpu::invalidateCode(at, 64);
            return cpu::bitsFloat(cpu::call(at, {cpu::floatBits(a), cpu::floatBits(b)}));
        };
        // vmov s0, r0; vmov s2, r1; <op>; vmov r0, s0
        const std::uint32_t load0 = 0xEE000A10, load2 = 0xEE011A10, store0 = 0xEE100A10;
        hlog("selftest", "vmin.f32 d0,d0,d1 (5,1) = %g (expect 1)", run({load0, load2, 0xF2200F01, store0}, 5, 1));
        hlog("selftest", "vmax.f32 d0,d0,d1 (5,1) = %g (expect 5)", run({load0, load2, 0xF2000F01, store0}, 5, 1));
        hlog("selftest", "vmul.f32 d0,d1,d0 (3,4) = %g (expect 12)", run({load0, load2, 0xF3010D10, store0}, 3, 4));
        hlog("selftest", "vadd.f32 d0,d0,d1 (3,4) = %g (expect 7)", run({load0, load2, 0xF2000D01, store0}, 3, 4));
        hlog("selftest", "vmul.f32 s0,s0,s2 (3,4) = %g (expect 12)", run({load0, load2, 0xEE200A01, store0}, 3, 4));
        return 0;
    }
    runtime::initialize(); // Objective-C class registration, +load, C++ static initializers
    hlog("startup", "initializers done elapsed_ms=%lld", elapsed());

    // crt1 start reads {argc, argv[], 0, envp[], 0, apple[], 0} from sp and calls main.
    const auto exePath = mem::heapString("/var/mobile/Applications/5C3A39B1-6A3B-4C2E-9D5A-0C1A2B3C4D5E/Forlorn.app/Forlorn");
    const auto applePath = mem::heapString("executable_path=/var/mobile/Applications/5C3A39B1-6A3B-4C2E-9D5A-0C1A2B3C4D5E/Forlorn.app/Forlorn");
    const std::uint64_t sp = (stackTop - 0x100) & ~7ull;
    const std::uint32_t words[] = {1, std::uint32_t(exePath), 0, 0, std::uint32_t(applePath), 0};
    for (std::size_t i = 0; i < 6; ++i) mem::w32(sp + 4 * i, words[i]);
    const auto result = cpu::runThread(loader::entryPoint(), {}, sp);
    hlog("startup", "main returned %lld", (long long)result);
    return int(result);
}
