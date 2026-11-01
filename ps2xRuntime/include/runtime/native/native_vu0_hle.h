#ifndef PS2X_NATIVE_VU0_HLE_H
#define PS2X_NATIVE_VU0_HLE_H

// A port's native versions of its game's VU0 microprograms (VCALLMS / VCALLMSR): asked first at each call, with the
// EE context (its VU0 registers: vu0_vf, vi, vu0_acc, vu0_q, vu0_i, the flags) and VU0's code and data memory; true,
// and the program is done, its results in the context and data memory as the program leaves them; false, and VU0's
// interpreter runs it (packetStats().vu0Interpreter counts those runs).
//
// PS2X_VU0_HLE_CHECK=1 (a diagnostic): each program the port runs natively runs on the interpreter too, from the same
// state; registers and data memory are compared and the differences reported per program (the interpreter's results
// kept).

#include <atomic>
#include <cstdint>

struct R5900Context;

namespace ps2x::native
{
    using Vu0Hle = bool (*)(R5900Context *ctx, const uint8_t *vu0Code, uint8_t *vu0Data, uint32_t startPc);

    inline std::atomic<Vu0Hle> g_vu0Hle{nullptr};
    inline void setVu0Hle(Vu0Hle hle) { g_vu0Hle.store(hle, std::memory_order_release); }
    inline Vu0Hle vu0Hle() { return g_vu0Hle.load(std::memory_order_acquire); }

    // Each DMA kick to VIF0 (channel 0), told to the port before the transfer: the registers the game wrote (CHCR, MADR,
    // TADR, QWC), with EE memory and VU0's data memory. The port can note from them what the game sends VU0 (which of
    // its microprogram images, from the game's own pointer to its chain) and, when the kick is completed unread
    // (setVif0Skipped: unread true), put in VU0's data memory what the game's transfer brings there.
    using Vif0Kick = void (*)(const uint8_t *rdram, uint8_t *vu0Data, uint32_t chcr, uint32_t madr, uint32_t tadr,
                              uint32_t qwc, bool unread);
    inline std::atomic<Vif0Kick> g_vif0Kick{nullptr};
    inline void setVif0Kick(Vif0Kick kick) { g_vif0Kick.store(kick, std::memory_order_release); }
    inline Vif0Kick vif0Kick() { return g_vif0Kick.load(std::memory_order_acquire); }

    // A port whose VU0 programs all run natively (Vu0Hle) and that writes VU0's data at its kicks (Vif0Kick): a DMA kick
    // to VIF0 completes at once, unread: no DMA tag followed, no VIFcode read, no MPG (VU0's code memory stays empty),
    // no UNPACK; QWC 0, STR cleared, D_STAT's channel 0 bit, the channel's DMAC handlers. A microprogram the port does
    // not run is then not run at all (no code to interpret): counted (packetStats().vu0NotRun) and reported once per pc.
    inline std::atomic<bool> g_vif0Skipped{false};
    inline void setVif0Skipped(bool on) { g_vif0Skipped.store(on, std::memory_order_release); }
    inline bool vif0Skipped() { return g_vif0Skipped.load(std::memory_order_acquire); }
}

#endif
