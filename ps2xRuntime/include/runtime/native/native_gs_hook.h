#ifndef PS2X_NATIVE_GS_HOOK_H
#define PS2X_NATIVE_GS_HOOK_H

// A native renderer drawing inside the GS stream: where the VIF1 interpreter skips a VU1 program that renderer draws
// itself (its PATH1 output never reaches the GS), GS::queueNativeDraw puts a marker in the GS queue, in PATH1 order.
// On the GS thread the marker calls the hook with the render target the GS output would have gone to, so the native
// draws sort with the GS draws around them (particles in front of a mesh, a HUD over it).

#include "runtime/gs/gs_types.h"

#include <atomic>
#include <cstdint>

namespace ps2x::native
{
    struct GsDrawTarget
    {
        // GL objects on the GS thread's context, current while the hook runs. A null framebuffer means the GS
        // renderer in use is not the native one (CPU, paraLLEl-GS): nothing to draw into.
        uint32_t framebuffer = 0, color = 0, depth = 0;
        uint32_t width = 0, height = 0; // GS pixels
        uint32_t scale = 1;             // the target's texels per GS pixel (GSNativeBackend's internal scale)
        // Depth is GS Z times zScale (1/2^32 Z32, 1/2^24 Z24, 1/2^16 Z16), larger nearer, GEQUAL; colour keeps the
        // GS alpha as stored (0x80 = 1.0).
        double zScale = 0.0;
        // The context's XYOFFSET (12.4 fixed point, as written) and SCISSOR (pixels, inclusive).
        uint32_t xOffset = 0, yOffset = 0;
        uint32_t scissorX0 = 0, scissorY0 = 0, scissorX1 = 0, scissorY1 = 0;
        // The context's drawing registers as written at the marker (raw GS layouts), for draws whose material is the
        // GS state where they sit in the stream (Material::fromGsContext): TEX0, TEXA, ALPHA, TEST, CLAMP, FOGCOL,
        // and the Z buffer's write mask.
        uint64_t tex0 = 0, texa = 0, alpha = 0, test = 0, clamp = 0;
        uint32_t fogcol = 0;
        bool zmask = false;
        uint32_t fbmsk = 0; // FRAME's write mask (bits kept): a draw whose alpha byte is masked leaves the target's alpha
    };

    // A texture a port decoded from its game's own assets (an offline pack), in place of the one TEX0 selects in GS
    // memory: RGBA8 rows, R in the low byte, alpha as the GS keeps it (0x80 = 1.0, as TextureCache::Texture and
    // packs::decodeRecord give it): the top-left part of the 2^TW x 2^TH texture the draw's texels reach. The port keeps it alive for as long as draws
    // may use it; key tells textures apart.
    struct NativeTexture
    {
        uint32_t width = 0, height = 0;
        const uint32_t *rgba = nullptr;
        uint64_t key = 0;
    };

    // A draw a port computes in the GS's own terms (a microprogram's output, translated to C++): vertices as the GS
    // takes them and the drawing registers the port knows, drawn at its group's point of the GS stream into the
    // render target there (Renderer::drawGs), by the GS renderer itself, with its textures, CLUTs and blending.
    struct GsVertex
    {
        float x = 0, y = 0; // primitive coordinates (the GS's 12.4 X and Y over 16)
        uint32_t z = 0;
        float s = 0, t = 0, q = 1.0f;
        uint32_t rgba = 0; // R in the low byte
        uint8_t fog = 0xFF;
    };
    struct GsMaterial
    {
        GSPrimReg prim{};
        GSTex0Reg tex0{};
        uint64_t tex1 = 0, clamp = 0, alpha = 0, test = 0, miptbp1 = 0, miptbp2 = 0;
        // The first draw with this TEX0 since the game set it: its CLUT load (CLD) happens here.
        bool loadClut = false;
        // Set: the draw samples this instead of GS memory (TEX0 still gives the wrap and size), and loads no CLUT.
        const NativeTexture *texture = nullptr;
    };

    using GsDrawHook = void (*)(uint64_t group, const GsDrawTarget &target);

    inline std::atomic<GsDrawHook> g_gsDrawHook{nullptr};
    inline void setGsDrawHook(GsDrawHook hook) { g_gsDrawHook.store(hook, std::memory_order_release); }
    inline GsDrawHook gsDrawHook() { return g_gsDrawHook.load(std::memory_order_acquire); }

    // A port whose native renderer draws and presents everything: the GS front end draws nothing. What the game sends
    // it only raises the events the CPU waits on (SIGNAL, FINISH, LABEL, at the time the GS would have reached them);
    // nothing is queued, drawn, uploaded or presented.
    inline std::atomic<bool> g_gsHeadless{false};
    inline void setGsHeadless(bool on) { g_gsHeadless.store(on, std::memory_order_release); }
    inline bool gsHeadless() { return g_gsHeadless.load(std::memory_order_acquire); }

    // What the fork still reads of the packets a game sends the GS and VIF1, counted where it reads them (a port logs
    // them: TMB's [tmb:packets]). With the DMA kicks to VIF1 and the GIF completed unread (setGsDmaSkipped) they stay 0.
    struct PacketStats
    {
        std::atomic<uint64_t> gifPackets{0}, gifBytes{0}; // GS::submitGIFPacket
        std::atomic<uint64_t> eventScans{0};               // GS::postQueuedCpuEvents (SIGNAL / FINISH / LABEL)
        std::atomic<uint64_t> signals{0}, finishes{0}, labels{0};
        std::atomic<uint64_t> gsIrqs{0};                   // GS interrupts (SIGNAL / FINISH) raised to the EE
        std::atomic<uint64_t> vif1Runs{0}, vif1Bytes{0}; // PS2Memory::processVIF1Data (the VIF1 interpreter)
        std::atomic<uint64_t> vif0Runs{0};                 // PS2Memory::processVIF0Data (VU0's code and data, not the GS's)
        std::atomic<uint64_t> vu0Interpreter{0};           // VU0 microprograms run on VU0's interpreter (not the port's Vu0Hle)
        std::atomic<uint64_t> vu0NotRun{0};                // ... not run at all: VIF0 unread (setVif0Skipped), no code
        std::atomic<uint64_t> chainWalks{0};               // PS2Memory::readDmaSourceChain, VIF1 / GIF (DMA tags followed)
        std::atomic<uint64_t> path3{0};                    // GIF channel transfers handed to the GIF arbiter
        std::atomic<uint64_t> hostGif{0};                  // libgraph HLE packets (sceGsExecLoadImage, sceGsPutDrawEnv...)
        std::atomic<uint64_t> kicks[10]{};                 // DMA channel starts
        std::atomic<uint64_t> unread[10]{};                // ... completed without reading their data (setGsDmaSkipped, setVif0Skipped)
    };
    inline PacketStats g_packetStats;
    inline PacketStats &packetStats() { return g_packetStats; }

    // A port that draws nothing from the GS packets (with setGsHeadless): a DMA kick to VIF1 (channel 1) or the GIF
    // (channel 2) completes at once, its chain and data not read: no DMA tag followed, no VIFcode or GIF tag
    // interpreted, no SIGNAL / FINISH / LABEL raised from them; the libgraph HLE (image transfers, the drawing
    // environment) sends nothing. CHCR.STR clears, D_STAT's channel bit rises and the channel's DMAC handlers run as
    // when the transfer ends on the console.
    inline std::atomic<bool> g_gsDmaSkipped{false};
    inline void setGsDmaSkipped(bool on) { g_gsDmaSkipped.store(on, std::memory_order_release); }
    inline bool gsDmaSkipped() { return g_gsDmaSkipped.load(std::memory_order_acquire); }
}

#endif
