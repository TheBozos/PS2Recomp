#include "ps2_coverage.h"

#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace ps2_coverage
{
    std::atomic<std::atomic<uint32_t> *> g_bits{nullptr};

    namespace
    {
        struct State
        {
            std::unique_ptr<std::atomic<uint32_t>[]> bits;
            std::string path;
            uint32_t intervalSeconds = 0;
            std::mutex writeMutex;
            std::mutex wakeMutex;
            std::condition_variable wake;
            bool stopping = false;
            std::thread writer;
        };

        std::mutex g_controlMutex;
        State *g_state = nullptr;

        bool writeState(State &state)
        {
            std::lock_guard<std::mutex> lock(state.writeMutex);
            const std::filesystem::path target(state.path);
            std::filesystem::path partial = target;
            partial += ".partial";
            uint32_t count = 0;
            {
                std::ofstream out(partial, std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;
                std::string text;
                text.reserve(64u * 1024u);
                char line[16];
                for (uint32_t word = 0; word < kWords; ++word)
                {
                    uint32_t value = state.bits[word].load(std::memory_order_relaxed);
                    while (value)
                    {
                        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(value));
                        value &= value - 1u;
                        std::snprintf(line, sizeof(line), "0x%08x\n", ((word << 5u) | bit) << 2u);
                        text += line;
                        ++count;
                    }
                }
                out << "# ps2x coverage v1\n# " << count << " guest PCs dispatched\n" << text;
                if (!out.flush())
                    return false;
            }
            std::error_code error;
            std::filesystem::rename(partial, target, error);
            if (error)
            {
                std::filesystem::remove(target, error);
                std::filesystem::rename(partial, target, error);
            }
            return !error;
        }

        void writerMain(State *state)
        {
            std::unique_lock<std::mutex> lock(state->wakeMutex);
            while (!state->stopping)
            {
                state->wake.wait_for(lock, std::chrono::seconds(state->intervalSeconds),
                                     [state] { return state->stopping; });
                if (state->stopping)
                    break;
                lock.unlock();
                writeState(*state);
                lock.lock();
            }
        }

        void stopAtExit()
        {
            stop();
        }
    }

    bool start(const std::string &path, uint32_t intervalSeconds)
    {
        std::lock_guard<std::mutex> lock(g_controlMutex);
        if (g_state || path.empty())
            return false;
        State *state = new State();
        state->bits = std::make_unique<std::atomic<uint32_t>[]>(kWords);
        state->path = path;
        state->intervalSeconds = intervalSeconds;
        g_state = state;
        g_bits.store(state->bits.get(), std::memory_order_release);
        if (intervalSeconds != 0u)
            state->writer = std::thread(writerMain, state);
        static bool s_registered = false;
        if (!s_registered)
        {
            s_registered = true;
            std::atexit(stopAtExit);
        }
        return true;
    }

    bool startFromEnvironment()
    {
        const char *path = std::getenv("PS2X_COVERAGE");
        if (!path || !*path || active())
            return false;
        uint32_t interval = 10u;
        if (const char *value = std::getenv("PS2X_COVERAGE_INTERVAL"); value && *value)
            interval = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
        const bool started = start(path, interval);
        if (started)
            std::cerr << "[coverage] recording dispatched guest PCs to " << path << std::endl;
        return started;
    }

    bool active() noexcept
    {
        return g_bits.load(std::memory_order_acquire) != nullptr;
    }

    bool ran(uint32_t pc) noexcept
    {
        std::atomic<uint32_t> *bits = g_bits.load(std::memory_order_acquire);
        if (!bits)
            return false;
        const uint32_t slot = (pc & (kRamBytes - 1u)) >> 2u;
        return (bits[slot >> 5u].load(std::memory_order_relaxed) & (1u << (slot & 31u))) != 0u;
    }

    bool write()
    {
        std::lock_guard<std::mutex> lock(g_controlMutex);
        return g_state && writeState(*g_state);
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(g_controlMutex);
        State *state = g_state;
        if (!state)
            return;
        {
            std::lock_guard<std::mutex> wakeLock(state->wakeMutex);
            state->stopping = true;
        }
        state->wake.notify_all();
        if (state->writer.joinable())
            state->writer.join();
        writeState(*state);
        g_bits.store(nullptr, std::memory_order_release);
        g_state = nullptr;
    }
}
