#include "Cpu.h"
#include "Hle.h"
#include "Loader.h"
#include "Log.h"
#include "Memory.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace cpu {
namespace {
Dynarmic::ExclusiveMonitor *g_monitor = nullptr;
std::atomic<std::size_t> g_nextProcessor{0};
constexpr std::size_t kMaxThreads = 64;
constexpr std::uint32_t kThumb = 0x20, kUserMode = 0x10;
std::uint64_t g_returnStub = 0, g_threadExitStub = 0;
std::mutex g_threadsLock;
std::vector<Thread *> g_threads;
} // namespace

struct Callbacks final : Dynarmic::A32::UserCallbacks {
    Thread *thread = nullptr;
    template <class T> T load(std::uint32_t a);
    template <class T> void store(std::uint32_t a, T v);
    std::uint8_t MemoryRead8(std::uint32_t a) override { return load<std::uint8_t>(a); }
    std::uint16_t MemoryRead16(std::uint32_t a) override { return load<std::uint16_t>(a); }
    std::uint32_t MemoryRead32(std::uint32_t a) override { return load<std::uint32_t>(a); }
    std::uint64_t MemoryRead64(std::uint32_t a) override { return load<std::uint64_t>(a); }
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { store(a, v); }
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { store(a, v); }
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { store(a, v); }
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { store(a, v); }
    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }
    template <class T> bool cas(std::uint32_t a, T v, T e) {
        if (!mem::valid(a, sizeof(T))) return false;
        auto *p = reinterpret_cast<std::atomic<T> *>(mem::base() + a);
        return p->compare_exchange_strong(e, v);
    }
    void InterpreterFallback(std::uint32_t pc, std::size_t n) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return ~0ull >> 2; }
};

// CP15: the user read-only thread ID register (TPIDRURO, MRC p15, 0, Rt, c13, c0, 3) holds the guest thread pointer.
struct Cp15 final : Dynarmic::A32::Coprocessor {
    std::uint32_t *tpidruro;
    explicit Cp15(std::uint32_t *t) : tpidruro(t) {}
    using CoprocReg = Dynarmic::A32::CoprocReg;
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override { return std::monostate{}; }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg crn, CoprocReg crm, unsigned opc2) override {
        if (!two && opc1 == 0 && crn == CoprocReg::C13 && crm == CoprocReg::C0 && opc2 == 3) return tpidruro;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
};

struct Thread {
    Callbacks callbacks;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::uint32_t tpidruro = 0;
    std::uint32_t pendingSvc = 0;
    bool svcPending = false, faulted = false;
    std::uint64_t faultAddress = 0;
    std::uint32_t lastHigh = 0;
    int depth = 0;
    std::uint64_t stackTop = 0;
};

namespace {
thread_local Thread *t_current = nullptr;
}

template <class T> T Callbacks::load(std::uint32_t a) {
    if (!mem::valid(a, sizeof(T))) {
        if (!thread->faulted) { thread->faulted = true; thread->faultAddress = a; thread->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2); }
        return T{};
    }
    return mem::read<T>(a);
}
template <class T> void Callbacks::store(std::uint32_t a, T v) {
    if (!mem::valid(a, sizeof(T))) {
        if (!thread->faulted) { thread->faulted = true; thread->faultAddress = a; thread->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2); }
        return;
    }
    mem::write<T>(a, v);
}
void Callbacks::InterpreterFallback(std::uint32_t pc, std::size_t n) {
    const bool thumb = thread->jit->Cpsr() & kThumb;
    fatal("unimplemented %s instruction at %s (%zu instructions, word %08x)", thumb ? "Thumb" : "ARM", loader::describe(pc).c_str(), n,
          mem::r32(pc));
}
void Callbacks::CallSVC(std::uint32_t swi) {
    thread->pendingSvc = swi;
    thread->svcPending = true;
    thread->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}
void Callbacks::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    const bool thumb = thread->jit->Cpsr() & kThumb;
    if (e == E::Yield || e == E::WaitForEvent || e == E::SendEvent || e == E::SendEventLocal || e == E::WaitForInterrupt ||
        e == E::PreloadData || e == E::PreloadDataWithIntentToWrite || e == E::PreloadInstruction) {
        // Hints: continue with the next instruction (2-byte hints only exist as 16-bit Thumb encodings).
        const bool narrow = thumb && (mem::read<std::uint16_t>(pc) & 0xF800) < 0xE800;
        thread->jit->Regs()[15] = pc + (thumb ? (narrow ? 2 : 4) : 4);
        return;
    }
    thread->jit->Regs()[15] = pc;
    backtrace(e == E::Breakpoint ? "BKPT (trap / assertion)" : "undefined instruction");
    fatal("guest exception %d at %s (%s)", int(e), loader::describe(pc).c_str(), thumb ? "Thumb" : "ARM");
}

void initProcess() { g_monitor = new Dynarmic::ExclusiveMonitor(kMaxThreads); }
void setReturnStub(std::uint64_t address) { g_returnStub = address; }
void setThreadExitStub(std::uint64_t address) { g_threadExitStub = address; }

Thread &attachThread(std::uint64_t stackTop, std::size_t codeCacheBytes) {
    auto *t = new Thread;
    t->callbacks.thread = t;
    t->stackTop = stackTop;
    // The thread pointer: libSystem keeps the pthread struct there; HLE code uses it as the TLS block.
    t->tpidruro = std::uint32_t(mem::calloc(0x1000));
    Dynarmic::A32::UserConfig config;
    config.callbacks = &t->callbacks;
    config.processor_id = g_nextProcessor++ % kMaxThreads;
    config.global_monitor = g_monitor;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(mem::base());
    config.coprocessors[15] = std::make_shared<Cp15>(&t->tpidruro);
    config.hook_hint_instructions = false;
    config.define_unpredictable_behaviour = true;
    config.enable_cycle_counting = false;
    config.always_little_endian = true;
    config.code_cache_size = codeCacheBytes;
    t->jit = std::make_unique<Dynarmic::A32::Jit>(config);
    t->jit->Regs()[13] = std::uint32_t(stackTop);
    t->jit->SetCpsr(kUserMode);
    // RunFast mode as on the device: flush-to-zero, default NaN.
    t->jit->SetFpscr(0x03000000);
    t_current = t;
    std::lock_guard lock(g_threadsLock);
    g_threads.push_back(t);
    return *t;
}
Thread &current() { return *t_current; }
std::uint64_t threadPointer() { return t_current ? t_current->tpidruro : 0; }
bool hasCurrent() { return t_current != nullptr; }

std::uint64_t x(int i) { return t_current->jit->Regs()[std::size_t(i)]; }
void setX(int i, std::uint64_t v) { t_current->jit->Regs()[std::size_t(i)] = std::uint32_t(v); }
std::uint64_t sp() { return x(13); }
void setSp(std::uint64_t value) { setX(13, value); }
std::uint64_t pc() { return x(15); }
void setPc(std::uint64_t value) {
    auto *jit = t_current->jit.get();
    const auto cpsr = jit->Cpsr();
    jit->SetCpsr((value & 1) ? (cpsr | kThumb) : (cpsr & ~kThumb));
    jit->Regs()[15] = std::uint32_t(value & ~1ull);
}
std::uint64_t lr() { return x(14); }
std::uint32_t current_svc() { return t_current->pendingSvc; }
std::uint32_t lastHigh() { return t_current->lastHigh; }
std::uint32_t floatBits(float value) { std::uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }
float bitsFloat(std::uint64_t bits) { const auto b = std::uint32_t(bits); float value; std::memcpy(&value, &b, 4); return value; }
void invalidateCode(std::uint64_t address, std::size_t length) {
    std::lock_guard lock(g_threadsLock);
    for (auto *t : g_threads) t->jit->InvalidateCacheRange(std::uint32_t(address), length);
}

// Runs until the stub at g_returnStub executes (the function returned).
static void runLoop() {
    Thread &t = *t_current;
    for (;;) {
        t.jit->Run();
        if (t.faulted) {
            t.faulted = false;
            backtrace("memory fault");
            fatal("invalid memory access at %08llx from %s", (unsigned long long)t.faultAddress, loader::describe(t.jit->Regs()[15]).c_str());
        }
        if (!t.svcPending) continue;
        t.svcPending = false;
        const auto swi = t.pendingSvc;
        if (swi == kReturnSvc) return;
        hle::dispatch(swi);
    }
}

std::uint64_t callWords(std::uint64_t function, const std::vector<std::uint32_t> &words) {
    Thread &t = *t_current;
    auto *jit = t.jit.get();
    // Save the interrupted guest context (we are inside an HLE handler, or at top level).
    const auto regs = jit->Regs();
    const auto ext = jit->ExtRegs();
    const auto cpsr = jit->Cpsr(), fpscr = jit->Fpscr();
    for (std::size_t i = 0; i < 4 && i < words.size(); ++i) jit->Regs()[i] = words[i];
    // Stack arguments below the interrupted frame, 8-byte aligned at the call.
    std::uint32_t sp = (regs[13] - 64) & ~7u;
    if (words.size() > 4) {
        sp = (sp - std::uint32_t(4 * (words.size() - 4))) & ~7u;
        for (std::size_t i = 4; i < words.size(); ++i) mem::w32(sp + 4 * (i - 4), words[i]);
    }
    jit->Regs()[13] = sp;
    jit->Regs()[14] = std::uint32_t(g_returnStub);
    setPc(function);
    ++t.depth;
    runLoop();
    --t.depth;
    const std::uint64_t result = jit->Regs()[0];
    t.lastHigh = jit->Regs()[1];
    jit->Regs() = regs;
    jit->ExtRegs() = ext;
    jit->SetCpsr(cpsr);
    jit->SetFpscr(fpscr);
    return result;
}
std::uint64_t call(std::uint64_t function, std::initializer_list<std::uint64_t> words) {
    std::vector<std::uint32_t> w;
    w.reserve(words.size());
    for (auto v : words) w.push_back(std::uint32_t(v));
    return callWords(function, w);
}

std::uint64_t runThread(std::uint64_t function, std::initializer_list<std::uint64_t> args, std::uint64_t stackPointer) {
    Thread &t = *t_current;
    t.jit->Regs()[7] = 0;
    if (!stackPointer) {
        t.jit->Regs()[13] = std::uint32_t(t.stackTop);
        return call(function, args);
    }
    // Entered with sp exactly at stackPointer (crt1 start reads argc/argv from it): no frame below it.
    for (std::size_t i = 0; i < args.size() && i < 4; ++i) t.jit->Regs()[i] = std::uint32_t(args.begin()[i]);
    t.jit->Regs()[13] = std::uint32_t(stackPointer);
    t.jit->Regs()[14] = std::uint32_t(g_returnStub);
    setPc(function);
    ++t.depth;
    runLoop();
    --t.depth;
    return t.jit->Regs()[0];
}

void backtrace(const char *why) {
    if (!t_current) return;
    auto *jit = t_current->jit.get();
    const auto &r = jit->Regs();
    std::fprintf(stderr, "[crash] %s\n[crash]   pc %s (%s)\n[crash]   lr %s\n", why, loader::describe(r[15]).c_str(),
                 (jit->Cpsr() & kThumb) ? "Thumb" : "ARM", loader::describe(r[14]).c_str());
    for (int i = 0; i < 16; i += 4) {
        std::fprintf(stderr, "[crash]  ");
        for (int k = i; k < i + 4; ++k) std::fprintf(stderr, " r%-2d=%08x", k, r[std::size_t(k)]);
        std::fprintf(stderr, "\n");
    }
    // Frame chain (iOS armv7): r7 -> {saved r7, return address}.
    std::uint64_t fp = r[7];
    for (int frame = 0; frame < 32 && fp && mem::valid(fp, 8); ++frame) {
        const auto ret = mem::r32(fp + 4);
        std::fprintf(stderr, "[crash]   #%d %s\n", frame, loader::describe(ret).c_str());
        const auto next = mem::r32(fp);
        if (next <= fp) break;
        fp = next;
    }
    std::fflush(stderr);
}
} // namespace cpu
