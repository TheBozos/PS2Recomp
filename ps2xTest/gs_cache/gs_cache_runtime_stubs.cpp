// The GS sources under test call into the runtime on SIGNAL/FINISH edges (ps2_runtime.cpp) and into the native
// renderer and texture cache; these suites link the GS alone.
void ps2NotifyGsCsrEdge() {}
#include "runtime/ps2_memory.h"

uint64_t ps2CurrentEeCycle() { return 0u; }
bool ps2ScheduleGsInterruptCheck(uint64_t) { return false; }
bool ps2OnVif1WorkerClock() { return false; }
uint32_t ps2GsPostEvent(GSRegisters &, GsCpuEvent, uint32_t, uint32_t, uint64_t, uint64_t) { return 0u; }

#include "runtime/native/native_renderer.h"
#include "runtime/native/native_texture_cache.h"

#include <cstdlib>

namespace ps2x::native
{
    bool uploadsAtSubmit() { return false; }
    bool uploadsInStreamOrder() { return false; }
    TextureCache &textures() { std::abort(); }
    void TextureCache::upload(uint64_t, uint64_t, uint64_t, const uint8_t *, size_t) {}
    void TextureCache::scanGifPacket(const uint8_t *, size_t) {}
    struct Renderer::Gpu
    {
    };
    Renderer::Renderer() {}
    Renderer::~Renderer() {}
    Renderer &Renderer::instance()
    {
        static Renderer renderer;
        return renderer;
    }
    bool Renderer::takeGsContextBatches(uint64_t, StreamVisitor, void *) { return false; }
}
