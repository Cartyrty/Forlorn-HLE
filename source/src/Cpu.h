#pragma once
// One dynarmic A32 JIT per guest thread (armv7, ARM and Thumb-2). Imported functions are ARM-mode stubs
// "SVC #n; BX LR": the SVC callback records n and halts the JIT, the run loop dispatches the HLE handler outside of
// JIT execution (so handlers may call back into guest code by re-entering the same JIT), and execution resumes at the
// BX LR. Host -> guest calls set LR to a stub holding SVC #kReturnSvc, which ends that nested run.
//
// iOS armv7 ABI: integer, pointer and float (softfp) arguments in r0-r3 then on the stack in 4-byte slots; 64-bit
// values take two consecutive words; results in r0 (and r1); structures larger than 4 bytes are returned through a
// hidden pointer in r0. r7 is the frame pointer.
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace Dynarmic::A32 { class Jit; }

namespace cpu {
constexpr std::uint32_t kReturnSvc = 0xFFFF;
constexpr std::uint32_t kThreadExitSvc = 0xFFFE;

struct Thread;
// Sets up the JIT for the calling host thread (each guest thread runs on its own host thread).
Thread &attachThread(std::uint64_t stackTop, std::size_t codeCacheBytes = 64 * 1024 * 1024);
// The guest thread pointer (pthread_self / TPIDRURO) of the calling thread.
std::uint64_t threadPointer();
Thread &current();
bool hasCurrent();
void initProcess();

// Register access for the current thread (valid inside HLE handlers): r0-r15.
std::uint64_t x(int index);
void setX(int index, std::uint64_t value);
std::uint64_t sp();
void setSp(std::uint64_t value);
std::uint64_t pc();
// Jumps to a guest function (a tail call, e.g. objc_msgSend); bit 0 selects Thumb.
void setPc(std::uint64_t value);
std::uint64_t lr();
// The SVC number being dispatched on this thread.
std::uint32_t current_svc();

// Calls a guest function: the words go to r0-r3, then the stack. Returns r0 (r1 is in lastHigh()).
std::uint64_t call(std::uint64_t function, std::initializer_list<std::uint64_t> words = {});
std::uint64_t callWords(std::uint64_t function, const std::vector<std::uint32_t> &words);
// r1 after the last call() on this thread (the high word of a 64-bit result).
std::uint32_t lastHigh();
// Runs the thread's top-level function until it returns (thread entry / crt1 start). stackPointer: the initial sp
// (0 = the top of the thread's stack).
std::uint64_t runThread(std::uint64_t function, std::initializer_list<std::uint64_t> args, std::uint64_t stackPointer = 0);

// Floats travel in integer registers (softfp).
std::uint32_t floatBits(float value);
float bitsFloat(std::uint64_t bits);

// The address of the stub holding SVC #kReturnSvc; set by the loader.
void setReturnStub(std::uint64_t address);
void setThreadExitStub(std::uint64_t address);
void invalidateCode(std::uint64_t address, std::size_t length);
void backtrace(const char *why);
} // namespace cpu
