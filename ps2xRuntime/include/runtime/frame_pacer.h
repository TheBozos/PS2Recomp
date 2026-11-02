#ifndef PS2_RUNTIME_FRAME_PACER_H
#define PS2_RUNTIME_FRAME_PACER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <thread>

#include "runtime/ps2_determinism.h"

// Native frame pacing: a game that opts in runs its own loop on real time instead of on the PS2's vblanks. Its frame
// end calls frameEnd(), which holds the frame to the cap (frames per second, 0 = unlimited: only the host's speed
// limits it) and returns the real time the frame took, which the game feeds to its engine as its step. The cap
// starts from PS2X_FRAME_CAP (default 60) and can change while the game runs. PS2X_FRAME_STATS=1: every 2 s, the
// time the game's frames took to make (from one frame end to the next, the wait for the cap left out): average,
// 95th percentile, worst, and how many went over the cap's period (frames a slower PC drops first).
// PS2X_DETERMINISTIC=1: the step returned is exactly one period of the cap (1/60 s with no cap), whatever the frame
// took; the frame is still held to the cap, which changes nothing the game computes.
class FramePacer
{
public:
    static constexpr double kMaxStepSeconds = 0.1; // a longer frame (a load, a debugger stop) counts as this much

    FramePacer()
    {
        if (const char *cap = std::getenv("PS2X_FRAME_CAP"); cap && *cap)
            m_cap.store(static_cast<uint32_t>(std::strtoul(cap, nullptr, 10)), std::memory_order_relaxed);
    }

    void setCap(uint32_t framesPerSecond) noexcept { m_cap.store(framesPerSecond, std::memory_order_relaxed); }
    [[nodiscard]] uint32_t cap() const noexcept { return m_cap.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t frames() const noexcept { return m_frames.load(std::memory_order_relaxed); }
    [[nodiscard]] double lastStepSeconds() const noexcept { return m_lastStep.load(std::memory_order_relaxed); }

    // Waits until the cap allows the next frame to start, then returns the time since the previous frame end.
    double frameEnd()
    {
        using Clock = std::chrono::steady_clock;
        const uint32_t cap = m_cap.load(std::memory_order_relaxed);
        Clock::time_point now = Clock::now();
        if (statsWanted() && m_frames.load(std::memory_order_relaxed) != 0u)
            recordWork(std::chrono::duration<double>(now - m_lastReturn).count(), cap, now);
        if (m_frames.load(std::memory_order_relaxed) == 0u)
            m_previous = now - std::chrono::microseconds(1000000 / std::max<uint32_t>(cap, 60u));
        if (cap != 0u)
        {
            const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / cap));
            const Clock::time_point due = m_previous + period;
            // Sleep most of the wait, then spin the last stretch: Windows sleeps overshoot by up to a millisecond.
            if (due - now > std::chrono::milliseconds(2))
                std::this_thread::sleep_until(due - std::chrono::milliseconds(1));
            while ((now = Clock::now()) < due)
                std::this_thread::yield();
        }
        const double step = ps2x::det::enabled() ? 1.0 / static_cast<double>(cap != 0u ? cap : 60u)
                                                 : std::min(std::chrono::duration<double>(now - m_previous).count(), kMaxStepSeconds);
        m_previous = now;
        m_lastReturn = Clock::now();
        m_lastStep.store(step, std::memory_order_relaxed);
        m_frames.fetch_add(1u, std::memory_order_relaxed);
        return step;
    }

private:
    static bool statsWanted()
    {
        static const bool wanted = []
        {
            const char *value = std::getenv("PS2X_FRAME_STATS");
            return value && *value == '1';
        }();
        return wanted;
    }

    void recordWork(double seconds, uint32_t cap, std::chrono::steady_clock::time_point now)
    {
        if (seconds > kMaxStepSeconds) // a load or a pause: not a frame's work
            return;
        const double ms = seconds * 1000.0;
        m_workSum += ms;
        m_workMax = std::max(m_workMax, ms);
        ++m_workHistogram[std::min<size_t>(static_cast<size_t>(ms * 4.0), kHistogramBins - 1u)];
        ++m_workFrames;
        if (cap != 0u && seconds > 1.0 / cap)
            ++m_workOver;
        if (m_statsSince == std::chrono::steady_clock::time_point{})
            m_statsSince = now;
        if (now - m_statsSince < std::chrono::seconds(2))
            return;
        uint64_t seen = 0u;
        size_t bin = 0u;
        while (bin + 1u < kHistogramBins && (seen += m_workHistogram[bin]) * 100u < m_workFrames * 95u)
            ++bin;
        std::fprintf(stderr, "[frame] %llu frames: work %.2f ms avg, %.2f p95, %.2f max; %llu over %.1f ms\n",
                     static_cast<unsigned long long>(m_workFrames), m_workSum / m_workFrames, (bin + 1u) / 4.0, m_workMax,
                     static_cast<unsigned long long>(m_workOver), cap ? 1000.0 / cap : 0.0);
        m_workSum = m_workMax = 0.0;
        m_workFrames = m_workOver = 0u;
        std::fill(std::begin(m_workHistogram), std::end(m_workHistogram), 0u);
        m_statsSince = now;
    }

    static constexpr size_t kHistogramBins = 400; // quarter milliseconds, to 100 ms
    std::atomic<uint32_t> m_cap{60};
    std::atomic<uint64_t> m_frames{0};
    std::atomic<double> m_lastStep{1.0 / 60.0};
    std::chrono::steady_clock::time_point m_previous{};
    std::chrono::steady_clock::time_point m_lastReturn{};
    // PS2X_FRAME_STATS (the game's thread only)
    std::chrono::steady_clock::time_point m_statsSince{};
    double m_workSum = 0.0, m_workMax = 0.0;
    uint64_t m_workFrames = 0u, m_workOver = 0u;
    uint32_t m_workHistogram[kHistogramBins] = {};
};

#endif // PS2_RUNTIME_FRAME_PACER_H
