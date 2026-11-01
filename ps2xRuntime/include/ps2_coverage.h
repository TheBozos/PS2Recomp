#ifndef PS2_COVERAGE_H
#define PS2_COVERAGE_H

#include <atomic>
#include <cstdint>
#include <string>

// PS2X_COVERAGE=<file>: the guest PCs the dispatcher looked up, one bit per word of EE RAM, written to <file>
// every PS2X_COVERAGE_INTERVAL seconds (default 10, 0: only at exit). tools/func_map reads it (--coverage).
namespace ps2_coverage
{
    constexpr uint32_t kRamBytes = 32u * 1024u * 1024u;
    constexpr uint32_t kWords = kRamBytes / 4u / 32u;

    extern std::atomic<std::atomic<uint32_t> *> g_bits;

    inline void note(uint32_t pc) noexcept
    {
        std::atomic<uint32_t> *bits = g_bits.load(std::memory_order_relaxed);
        if (!bits)
            return;
        const uint32_t slot = (pc & (kRamBytes - 1u)) >> 2u;
        const uint32_t mask = 1u << (slot & 31u);
        std::atomic<uint32_t> &word = bits[slot >> 5u];
        if ((word.load(std::memory_order_relaxed) & mask) == 0u)
            word.fetch_or(mask, std::memory_order_relaxed);
    }

    bool start(const std::string &path, uint32_t intervalSeconds);
    bool startFromEnvironment();
    bool active() noexcept;
    bool ran(uint32_t pc) noexcept;
    bool write();
    void stop();
}

#endif
