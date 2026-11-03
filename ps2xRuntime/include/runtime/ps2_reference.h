#ifndef PS2_RUNTIME_REFERENCE_H
#define PS2_RUNTIME_REFERENCE_H

// The reference recorder (PS2X_REFERENCE=<file.ref>): one record per game frame (EeScheduler::presentGuestFrame, the
// game handing its picture over) of what the guest computed, to compare two runs (or this runtime against another
// one) frame for frame. With PS2X_DETERMINISTIC=1 (ps2_determinism.h) two runs with the same inputs write the same
// file. Per frame: the frame number, XXH64 hashes of the 32 MB of EE RAM and the 16 KB scratchpad, of the frame's
// kernel and library calls (id, a0-a3, result; the dispatcher's calls to the addresses the port names with
// setCallNames, and raw syscalls), of the native renderer's draw lists (material, matrix, vertices, texture content),
// of the sound mixed during the frame, and of the EE thread state (running thread, ready order). The layout, and the
// side files (full call lists, RAM dumps), are described in the port's docs (TMB_Recomp: docs/REFERENCE_HARNESS.md).
// Hashes never include a host pointer: textures are identified by their content.
//
// PS2X_REFERENCE_FRAMES=<n>: the run ends (the file closed, the process exits 0) once n records are written.
// PS2X_REFERENCE_CALLS=<n> (default 120): the full call lists of the first n frames go to <file>.calls;
// PS2X_REFERENCE_CALLS_FROM=<f>: from frame f instead of 0. PS2X_REFERENCE_DUMP=<f>[-<g>][,...]: the RAM and
// scratchpad of those frames to <file>.ram.<frame> (for the compare tool's first differing block).
// PS2X_REFERENCE_MASK=<addr>:<bytes>[,...]: RAM ranges hashed as zeros (listed in the header). PS2X_REFERENCE_RUN=<name>:
// the run's name in the header (default: the file's stem).

#include <cstddef>
#include <cstdint>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace ps2x::reference
{
    // XXH64 (seed as given), the recorder's hash.
    uint64_t hash64(const void *data, size_t bytes, uint64_t seed = 0u) noexcept;

    bool active() noexcept;

    // The game frame that just ended (EeScheduler::presentGuestFrame): writes its record.
    void frameEnd(PS2Runtime &runtime);

    // The guest addresses whose calls are recorded (the port's library and kernel stubs), with their names: a call's
    // id is the FNV-1a 32 hash of its name. Calls through the dispatcher only (JAL / JALR to those addresses).
    struct CallName
    {
        uint32_t address;
        const char *name;
    };
    void setCallNames(const CallName *names, size_t count);
    // Set while calls are recorded (active and names given): the dispatcher's only test when they are not.
    extern bool g_tracingCalls;
    bool tracesAddress(uint32_t pc) noexcept;
    // Called by the dispatcher in place of the call: records it, runs it, records its result (or that it blocked).
    using GuestFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    void tracedCall(uint32_t pc, GuestFunction function, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // A raw syscall (PS2Runtime::handleSyscall): id 0x80000000 | (number & 0x7FFFFFFF); the result as it returns.
    size_t noteSyscallEntry(uint32_t number, const R5900Context *ctx);
    void noteSyscallResult(size_t slot, const R5900Context *ctx, bool blocked);

    // The native renderer's draw lists published during the frame (each list's hash, batches and vertices): the
    // frame's 3D list (Renderer::endFrame), its 2D overlay (publishOverlay), a movie picture (setBackdrop).
    void noteDraws(uint64_t hash, uint32_t batches, uint32_t vertices);

    // A build name for the header (default: the runtime's compile date); the port's version, say.
    void setBuild(const char *build);

    // The call log (ps2_calllog.h): the frame's traced calls (the record's calls field) made inside an out-call, and,
    // replaying, the same put back. flags: the record's flags (1 blocked, 2 open when the frame ended), bit 31 set once
    // the call returned or blocked.
    struct TracedCall
    {
        uint32_t id, pc, a[4], v0, flags;
    };
    size_t tracedCallCount() noexcept;
    void tracedCallsSince(size_t from, std::vector<TracedCall> &out);
    void injectTracedCalls(const TracedCall *calls, size_t count);
}

#endif // PS2_RUNTIME_REFERENCE_H
