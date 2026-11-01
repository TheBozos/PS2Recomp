#ifndef PS2X_STORE_PATCH_H
#define PS2X_STORE_PATCH_H

// A port's patch of its game's register stores (the stores the recompiled code makes through PS2Runtime::Store32, the
// hardware registers'): the hook sees the instruction's address (ctx->pc), the target and the value, and returns true
// for a store the game is not to make, as if the instruction were patched out (a DMA kick of something the port draws
// itself, inline in a function too large to rewrite).

#include <atomic>
#include <cstdint>

namespace ps2x
{
    using StorePatch = bool (*)(uint32_t pc, uint32_t address, uint32_t value);

    inline std::atomic<StorePatch> g_storePatch{nullptr};
    inline void setStorePatch(StorePatch patch) { g_storePatch.store(patch, std::memory_order_release); }
    inline StorePatch storePatch() { return g_storePatch.load(std::memory_order_acquire); }
}

#endif
