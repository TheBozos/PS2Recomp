// The out-of-line half of ps2_recomp_abi.h: the runtime calls of recompiled code, forwarded to PS2Runtime
// (ps2rCheckpointDue is in ps2_runtime.cpp, next to the member it implements).

#include "ps2_recomp_abi.h"
#include "ps2_runtime.h"
#include "runtime/ps2_capture.h"

// Generated code is compiled against R5900Context from ps2_recomp_abi.h; the build recompiles it when that header
// changes, but an object built elsewhere would not be. A layout change must update this size (and regenerate).
static_assert(sizeof(R5900Context) == 1648, "R5900Context layout changed: update the size and rebuild all generated code");

namespace
{
    namespace cap = ps2x::capture;

    // PS2X_CAPTURE: a load from an address that is not memory is an out-call (its value recorded, or answered from the
    // capture in the self-check). Memory reached through here (the scratchpad, VU0's data memory) is not.
    template <typename T, typename Load>
    PS2_ALWAYS_INLINE T captureLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t vaddr, Load &&load)
    {
        if (!cap::g_active || cap::plainMemory(vaddr)) [[likely]]
            return load();
        T value{};
        cap::NativeScope scope;
        if (cap::ioEnter(rdram, ctx, runtime, cap::kOutIoLoad, vaddr, sizeof(T), &value, scope.token))
            return value;
        value = load();
        cap::ioValue(scope.token, &value, sizeof(T));
        return value;
    }

    template <typename T, typename Store>
    PS2_ALWAYS_INLINE void captureStore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t vaddr, T value,
                                        Store &&store)
    {
        if (!cap::g_active || cap::plainMemory(vaddr)) [[likely]]
        {
            store();
            return;
        }
        cap::NativeScope scope;
        if (cap::ioEnter(rdram, ctx, runtime, cap::kOutIoStore, vaddr, sizeof(T), &value, scope.token))
            return;
        store();
    }

    // Any other runtime call of generated code: an out-call of the given kind.
    template <typename Call>
    PS2_ALWAYS_INLINE void captureCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, cap::OutKind kind,
                                       uint32_t target, Call &&call)
    {
        if (!cap::g_active) [[likely]]
        {
            call();
            return;
        }
        cap::NativeScope scope;
        if (cap::nativeEnter(rdram, ctx, runtime, kind, target, scope.token))
            return;
        call();
    }
}

uint8_t ps2rLoad8(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    return captureLoad<uint8_t>(rdram, ctx, runtime, vaddr, [&] { return runtime->Load8(rdram, ctx, vaddr); });
}
uint16_t ps2rLoad16(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    return captureLoad<uint16_t>(rdram, ctx, runtime, vaddr, [&] { return runtime->Load16(rdram, ctx, vaddr); });
}
uint32_t ps2rLoad32(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    return captureLoad<uint32_t>(rdram, ctx, runtime, vaddr, [&] { return runtime->Load32(rdram, ctx, vaddr); });
}
uint64_t ps2rLoad64(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    return captureLoad<uint64_t>(rdram, ctx, runtime, vaddr, [&] { return runtime->Load64(rdram, ctx, vaddr); });
}
__m128i ps2rLoad128(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    return captureLoad<__m128i>(rdram, ctx, runtime, vaddr, [&] { return runtime->Load128(rdram, ctx, vaddr); });
}

void ps2rStore8(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    captureStore(rdram, ctx, runtime, vaddr, value, [&] { runtime->Store8(rdram, ctx, vaddr, value); });
}
void ps2rStore16(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    captureStore(rdram, ctx, runtime, vaddr, value, [&] { runtime->Store16(rdram, ctx, vaddr, value); });
}
void ps2rStore32(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    captureStore(rdram, ctx, runtime, vaddr, value, [&] { runtime->Store32(rdram, ctx, vaddr, value); });
}
void ps2rStore64(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    captureStore(rdram, ctx, runtime, vaddr, value, [&] { runtime->Store64(rdram, ctx, vaddr, value); });
}
void ps2rStore128(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    captureStore(rdram, ctx, runtime, vaddr, value, [&] { runtime->Store128(rdram, ctx, vaddr, value); });
}

bool ps2rDispatchGuestBranch(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t targetPc, uint32_t sourcePc,
                             uint32_t fallthroughPc, Ps2GuestBranchKind kind, const char *debugName)
{
    return runtime->dispatchGuestBranch(rdram, ctx, targetPc, sourcePc, fallthroughPc, kind, debugName);
}

void ps2rSignalException(PS2Runtime *runtime, R5900Context *ctx, PS2Exception exception)
{
    captureCall(runtime->memory().getRDRAM(), ctx, runtime, cap::kOutException, ctx->pc,
                [&] { runtime->SignalException(ctx, exception); });
}
void ps2rExecuteVU0Microprogram(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    captureCall(rdram, ctx, runtime, cap::kOutVu0, address, [&] { runtime->executeVU0Microprogram(rdram, ctx, address); });
}
void ps2rVu0StartMicroProgram(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    captureCall(rdram, ctx, runtime, cap::kOutVu0, address, [&] { runtime->vu0StartMicroProgram(rdram, ctx, address); });
}
uint8_t *ps2rVu0Data(PS2Runtime *runtime) { return runtime->memory().getVU0Data(); }

void ps2rHandleSyscall(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutSyscall, ctx->pc, [&] { runtime->handleSyscall(rdram, ctx); });
}
void ps2rHandleSyscall(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    captureCall(rdram, ctx, runtime, cap::kOutSyscall, ctx->pc,
                [&] { runtime->handleSyscall(rdram, ctx, encodedSyscallId); });
}
void ps2rHandleBreak(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutException, ctx->pc, [&] { runtime->handleBreak(rdram, ctx); });
}
void ps2rHandleTrap(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutException, ctx->pc, [&] { runtime->handleTrap(rdram, ctx); });
}
void ps2rHandleTLBR(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutException, ctx->pc, [&] { runtime->handleTLBR(rdram, ctx); });
}
void ps2rHandleTLBWI(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutException, ctx->pc, [&] { runtime->handleTLBWI(rdram, ctx); });
}
void ps2rHandleTLBWR(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutException, ctx->pc, [&] { runtime->handleTLBWR(rdram, ctx); });
}
void ps2rHandleTLBP(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx)
{
    captureCall(rdram, ctx, runtime, cap::kOutException, ctx->pc, [&] { runtime->handleTLBP(rdram, ctx); });
}
void ps2rClearLLBit(PS2Runtime *runtime, R5900Context *ctx) { runtime->clearLLBit(ctx); }
// COP0 Count and the performance counters: no generated code of TMB uses them; a capture marks a sample that does as
// not replayable.
uint32_t ps2rReadCop0Count(PS2Runtime *runtime)
{
    cap::noteUnreplayable();
    return runtime->readCop0Count();
}
void ps2rWriteCop0Count(PS2Runtime *runtime, uint32_t value)
{
    cap::noteUnreplayable();
    runtime->writeCop0Count(value);
}
uint32_t ps2rReadCop0Perf(PS2Runtime *runtime, uint32_t select)
{
    cap::noteUnreplayable();
    return runtime->readCop0Perf(select);
}
void ps2rWriteCop0Perf(PS2Runtime *runtime, uint32_t select, uint32_t value)
{
    cap::noteUnreplayable();
    runtime->writeCop0Perf(select, value);
}

void ps2rKickGifDmaChainFromMMIO(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t dPcrValue,
                                 uint32_t dStatValue, uint32_t tadr, uint32_t chcr)
{
    // The store to D2_CHCR it stands for (GIF DMA channel control).
    captureCall(rdram, ctx, runtime, cap::kOutIoStore, 0x1000A000u,
                [&] { runtime->kickGifDmaChainFromMMIO(rdram, ctx, dPcrValue, dStatValue, tadr, chcr); });
}
