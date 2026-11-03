#ifndef PS2_RUNTIME_CAPTURE_H
#define PS2_RUNTIME_CAPTURE_H

// The per-function capture (PS2X_CAPTURE=<folder>, deterministic mode only): samples of guest function calls, each
// with the full guest context and memory at entry, every call it makes out of guest code (stubs, port overrides,
// syscalls, VU0 microprograms, hardware loads and stores) with what each changed, and the context and memory changes
// at exit, so another recompiler can run the same function from the same state and compare the result. The format
// is described without this code in BozoEngine's docs/specs/capture.md.
//
// Needs code generated with capture_hooks (ps2_recomp_abi.h: PS2X_CAPTURE_GUEST / PS2X_CAPTURE_NATIVE).
//
// PS2X_CAPTURE=<folder>: capture.bin (the samples) and summary.txt there.
// PS2X_CAPTURE_SAMPLES=<n> (default 4): replayable samples per function, spread over the frame window: sample i is
//   taken at the function's first call (that does not fail) from frame from + i * (to - from) / n.
// PS2X_CAPTURE_FRAMES=<from>-<to>: the frame window (default 0 to PS2X_REFERENCE_FRAMES, else 0-18000).
// PS2X_CAPTURE_FUNCS=<address>[,...]: only these functions.
// PS2X_CAPTURE_TRIES=<n> (default 12): unreplayable attempts per function before it is given up.
// PS2X_CAPTURE_CHECK=<folder>: the self-check instead of the game: every replayable sample of <folder>/capture.bin
//   re-run through this build's own generated functions, out-calls answered from the capture; the exit state must
//   match bit for bit. PS2X_CAPTURE_CHECK_LOG=<n> (default 20): mismatches printed in detail.

#include <cstddef>
#include <cstdint>

#include "ps2_recomp_abi.h"

class PS2Runtime;

namespace ps2x::capture
{
    // Reads the environment once (the first call); true when capturing or replaying.
    bool configure() noexcept;
    bool replaying() noexcept;

    // Main RAM, allocated so that writes to it can be tracked (Windows: MEM_WRITE_WATCH) when the capture is on.
    uint8_t *allocateRam(size_t bytes);
    void freeRam(uint8_t *ram) noexcept;

    // Not memory for the capture: a load or store through the runtime to this address is an out-call.
    bool plainMemory(uint32_t vaddr) noexcept;
    bool ioEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, OutKind kind, uint32_t address,
                 uint32_t size, void *value, uint32_t &token);
    void ioValue(uint32_t token, const void *value, uint32_t size) noexcept;
    // The open sample cannot be replayed (a COP0 timer read...).
    void noteUnreplayable() noexcept;

    // PS2Runtime::replaceFunction: a port function bound to a guest address (calls to it are out-calls).
    void noteFunctionReplaced(uint32_t address, void *original, void *replacement) noexcept;
    bool isOverride(uint32_t address) noexcept;

    // ps2rCheckpointDue: the scheduler's answer when capturing (a checkpoint that fires inside a sample makes it
    // unreplayable); never due in the self-check.
    bool checkpoint(bool due) noexcept;
    bool replayCheckpoint(PS2Runtime *runtime) noexcept;

    // EeScheduler::run. A sampled call whose code jumps out through the dispatcher (an indirect tail jump) stays open
    // while the dispatcher runs its continuation, and ends when the guest context reaches its return address with its
    // stack pointer: schedulerPass before the dispatcher's own work (any makes the sample unreplayable),
    // schedulerResume with the context it is about to run, noteDispatcherOddity on its other paths.
    void schedulerPass(bool workPending) noexcept;
    void schedulerResume(R5900Context *context, int thread) noexcept;
    void noteDispatcherOddity() noexcept;

    // The game frame that ended (EeScheduler::presentGuestFrame), and the end of the run (the file completed).
    void frameEnd() noexcept;
    void finish() noexcept;

    bool selfCheckRequested() noexcept;
    int runSelfCheck(PS2Runtime &runtime);
}

#endif // PS2_RUNTIME_CAPTURE_H
