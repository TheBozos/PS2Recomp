#ifndef PS2X_NATIVE_DISPLAY_H
#define PS2X_NATIVE_DISPLAY_H

// A port with no GS front end at all (setGsFrontEndAbsent): its native renderer draws and presents every frame, and
// what a game still asks of "the display" comes from here, native state on the PC's clock, not a GS register model:
// - the field the display shows (CSR.FIELD, which games read to know which field a vblank ended), from the vblanks
//   the EE scheduler counts on the frame pacer's clock (displayVblank);
// - the frames the game ended (displayFrameEnded, the FPS counter's), from its frame end (EeScheduler::presentGuestFrame);
// - the display mode the game set (interlace, field / frame mode, PAL), from sceGsResetGraph / SetGsCrt;
// - the interrupt mask the game asked for (GsPutIMR, returned by GsGetIMR): there is no GS to interrupt it.
// Then the runtime constructs no GS object (no GS thread, no raster backend, no OpenGL context of its own), allocates
// no GS memory, and no libgraph HLE call, kernel call or guest access to the GS registers reaches a GS: the guest's
// reads of the privileged registers get displayCsr() (CSR) or 0, its writes are counted and dropped.
//
// Set before the runtime is constructed (a port's static initialiser: the PS2Runtime constructor decides whether to
// construct the GS front end, PS2Memory::initialize whether to allocate GS memory); never changed after.
//
// PS2X_GS_TOUCH(name) counts, by call site, each time something reaches the GS front end, GS memory or the GS
// registers ("gs: ..." sites) or the native display that replaced them ("display: ..." sites); a port lists them
// (forEachGsTouch: TMB's [tmb:gs]). A site appears once it has been reached.

#include <atomic>
#include <cstdint>

namespace ps2x::native
{
    inline std::atomic<bool> g_gsFrontEndAbsent{false};
    inline void setGsFrontEndAbsent(bool on) { g_gsFrontEndAbsent.store(on, std::memory_order_release); }
    inline bool gsFrontEndAbsent() { return g_gsFrontEndAbsent.load(std::memory_order_acquire); }

    struct GsTouchSite;
    inline std::atomic<GsTouchSite *> g_gsTouchSites{nullptr};
    struct GsTouchSite
    {
        const char *name;
        std::atomic<uint64_t> count{0};
        GsTouchSite *next = nullptr;
        explicit GsTouchSite(const char *siteName) : name(siteName)
        {
            next = g_gsTouchSites.load(std::memory_order_relaxed);
            while (!g_gsTouchSites.compare_exchange_weak(next, this, std::memory_order_release, std::memory_order_relaxed))
            {
            }
        }
    };
    template <typename F>
    void forEachGsTouch(F &&visit)
    {
        for (GsTouchSite *site = g_gsTouchSites.load(std::memory_order_acquire); site; site = site->next)
            visit(site->name, site->count.load(std::memory_order_relaxed));
    }

    struct DisplayMode
    {
        bool interlace = true; // SMODE2.INT
        bool frameMode = false; // SMODE2.FFMD: frame (each field shows every line of the buffer) or field
        bool pal = false;
    };

    struct DisplayState
    {
        std::atomic<uint64_t> vblanks{0}; // vblank starts since boot (EeScheduler, on the frame pacer's clock)
        std::atomic<uint64_t> frames{0};  // frames the game ended (presentGuestFrame)
        std::atomic<uint32_t> mode{1u};   // DisplayMode packed: bit 0 interlace, 1 frame mode, 2 PAL
        std::atomic<uint64_t> imr{0};     // GsPutIMR's mask, for GsGetIMR
    };
    inline DisplayState g_display;
    inline DisplayState &display() { return g_display; }

    inline void displayVblank(uint64_t tick) { g_display.vblanks.store(tick, std::memory_order_release); }
    // CSR.FIELD: set on odd vblanks (the odd field shown), as the console's display toggles it.
    inline bool displayField() { return (g_display.vblanks.load(std::memory_order_acquire) & 1u) != 0u; }
    // The CSR a guest read gets: FIELD (bit 13) and FIFO empty (bits 14-15 = 01: nothing is ever queued for a GS);
    // SIGNAL, FINISH, HSINT, VSINT, EDWINT, REV and ID 0.
    inline uint64_t displayCsr() { return (displayField() ? 0x2000ull : 0u) | 0x4000ull; }
    inline void displayFrameEnded() { g_display.frames.fetch_add(1u, std::memory_order_relaxed); }
    inline uint64_t displayFrames() { return g_display.frames.load(std::memory_order_relaxed); }
    inline void setDisplayMode(const DisplayMode &mode)
    {
        g_display.mode.store((mode.interlace ? 1u : 0u) | (mode.frameMode ? 2u : 0u) | (mode.pal ? 4u : 0u),
                             std::memory_order_relaxed);
    }
    inline DisplayMode displayMode()
    {
        const uint32_t packed = g_display.mode.load(std::memory_order_relaxed);
        DisplayMode mode;
        mode.interlace = (packed & 1u) != 0u;
        mode.frameMode = (packed & 2u) != 0u;
        mode.pal = (packed & 4u) != 0u;
        return mode;
    }
    // SMODE2's INT / FFMD bits as libgraph's display environments hold them.
    inline uint64_t displaySmode2()
    {
        const DisplayMode mode = displayMode();
        return (mode.interlace ? 1u : 0u) | (mode.frameMode ? 2u : 0u);
    }
}

#define PS2X_GS_TOUCH(siteName)                                                     \
    do                                                                              \
    {                                                                               \
        static ::ps2x::native::GsTouchSite s_gsTouchSite_(siteName);                \
        s_gsTouchSite_.count.fetch_add(1u, std::memory_order_relaxed);              \
    } while (0)

#endif
