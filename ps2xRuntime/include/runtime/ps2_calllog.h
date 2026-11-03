#ifndef PS2_RUNTIME_CALLLOG_H
#define PS2_RUNTIME_CALLLOG_H

// The call log (PS2X_CALL_LOG=<file>, deterministic mode only): the whole run as BozoEngine's phase 2 needs it to
// stand in for every library it does not implement yet. In execution order, on the EE thread:
// - every call out of guest code (stubs, port overrides, syscalls, VU0 microprograms, hardware loads and stores), with
//   its point (frame, guest thread, EE cycle), its inputs and the context words and memory it changed;
// - every change to guest-visible state made outside guest code and outside any out-call: the IOP's writes into EE
//   memory (SIF DMA, RPC replies, SIF command packets), the kernel's and the libraries' continuations run by the
//   dispatcher (vblank flags, wait and handler completions), each pinned to the EE cycle and the out-call before it;
// - the context of every guest thread and handler as the dispatcher resumes it (the words changed since it last ran),
//   every handler invocation started and ended, every thread switch with its reason, the scheduler operations the
//   runtime made (invocations queued, threads blocked and woken, interrupts raised), and the frame ends.
// The format is BozoEngine's docs/specs/calllog.md ("PS2XCLG1"), written without this code. Needs code generated with
// capture_hooks (ps2_recomp_abi.h), like the per-function capture.
//
// PS2X_CALL_LOG=<file>: record. PS2X_CALL_LOG_STRICT=1: no lazy windows (the dispatcher's memory is compared after every
//   guest run, the IOP's after every IOP slice); its log must equal the lazy one (the self-check of the windows).
// PS2X_CALL_LOG_CHECK=<file>: the self-check against a recorded log: the run made again, every record it produces
//   compared with the log's, and every out-call's and asynchronous window's effects on guest memory and context
//   replaced by the log's (the bytes and words the run itself wrote are thrown away first). The run's reference record
//   (PS2X_REFERENCE) must still equal the one recorded without it. Report in <file>.check.txt.

#include <cstddef>
#include <cstdint>

#include "ps2_recomp_abi.h"

class PS2Runtime;
class EeScheduler;
struct GuestThread;
struct GuestInvocation;
struct EeWaitState;

namespace ps2x::reference
{
    struct CallName;
}

namespace ps2x::calllog
{
    extern bool g_on; // recording or checking

    // Reads the environment once; true when the call log is on (deterministic mode).
    bool configure() noexcept;

    // Names of the stub addresses (ps2x::reference::setCallNames) and the overrides (capture's list), for the file.
    void setCallNames(const ps2x::reference::CallName *names, size_t count);
    void overridesChanged() noexcept;

    // Out-calls (ps2_capture's hooks forward here while the call log is on).
    bool callEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, capture::OutKind kind, uint32_t target,
                   uint32_t &token);
    void callLeave(uint32_t token) noexcept;
    bool ioEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, capture::OutKind kind, uint32_t address,
                 uint32_t size, void *value, uint32_t &token);
    void ioValue(uint32_t token, const void *value, uint32_t size) noexcept;

    // EeScheduler.
    void bind(PS2Runtime *runtime) noexcept;
    void checkpoint() noexcept;                 // every checkpointDue
    // A stretch of dispatcher work that may change guest memory or contexts (source: what it is, calllog.md).
    void phaseBegin(uint32_t source) noexcept;
    void phaseEnd() noexcept;
    bool strict() noexcept;
    void beforeGuest(EeScheduler &scheduler, int thread, uint32_t depth, R5900Context &context) noexcept;
    void afterGuest(EeScheduler &scheduler, int thread, uint32_t depth) noexcept;
    void invocationStarted(int thread, uint32_t depth, const GuestInvocation &invocation) noexcept;
    void invocationEnded(int thread, uint32_t depth, const GuestInvocation &invocation) noexcept;
    // Scheduler operations (calllog.md, OP records).
    enum Op : uint32_t
    {
        kOpQueue = 1,      // queueInvocation: a handler or callback to run on its own (invocation fields)
        kOpBlock = 2,      // the running thread blocks (wait reason and object)
        kOpWake = 3,       // a waiting thread is made ready (thread, result)
        kOpIrq = 4,        // an interrupt raised (INTC or DMAC cause)
        kOpTask = 5,       // a host task posted to run at the next dispatcher pass
        kOpEvent = 6,      // a scheduler event posted (type, id, value)
        kOpCreate = 7,     // a guest thread created (id, entry, stack, priority)
        kOpStart = 8,      // a guest thread started (id, argument)
        kOpExit = 9,       // the running thread ends (ExitThread / ExitDeleteThread / return from its entry)
        kOpTerminate = 10, // a thread terminated or deleted by another
        // The kernel's other state changes, made by whoever calls the scheduler (a kernel call, a port override, a
        // library, the IOP): 11 priority (id, priority), 12 rotate (priority), 13 suspend (id), 14 resume (id),
        // 15 wakeup (id), 16 cancel wakeup (id), 17 release wait (id), 18 semaphore create (init, max, attr, option),
        // 19 semaphore delete (id), 20 signal (id), 21 poll (id), 22 wait (id), 23 event flag create (bits, attr,
        // option), 24 delete (id), 25 set (id, bits), 26 clear (id, mask), 27 poll (id, bits, mode), 28 wait (id,
        // bits, mode, result address), 29 alarm set (ticks, handler, argument, gp, sp), 30 alarm cancel (id),
        // 31 handler add (dmac, cause, handler, append, argument, gp, sp), 32 handler remove (dmac, cause, id),
        // 33 handler enable (dmac, id, on), 34 cause enable (dmac, cause, on), 35 vsync flag (flag, tick address),
        // 36 GS vsync callback (callback, gp, sp), 37 field rate (numerator, denominator), 38 thread setup (stack,
        // size, gp), 39 transfer if requested (interrupt safe).
    };
    void op(uint32_t kind, const uint32_t *args, uint32_t count, uint64_t wide = 0) noexcept;
    void opQueue(uint32_t kind, const GuestInvocation &invocation) noexcept;
    void opBlock(int thread, const EeWaitState &wait) noexcept;
    // A task posted by the dispatcher runs (processEvent Task): the operations it makes are its own origin.
    void taskBegin() noexcept;
    void taskEnd() noexcept;

    // The IOP (PS2IopHostAdapter: any call that can change EE memory or the EE scheduler) and the end of an IOP slice
    // (PS2Runtime::advanceIopEeCycles; site 1: EE cycles accounted, 2: the vblank's catch-up).
    void iopTouch() noexcept;
    void iopBegin() noexcept;
    void iopEnd(uint32_t site) noexcept;

    void frameEnd() noexcept;
    void finish() noexcept;
}

#endif // PS2_RUNTIME_CALLLOG_H
