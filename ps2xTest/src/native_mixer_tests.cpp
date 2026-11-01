#include "MiniTest.h"
#include "ps2x/iop/native_mixer.h"

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

using ps2x::audio::NativeMixer;

namespace
{
    constexpr uint32_t kBlockBytes = 1024u, kRingBytes = 4u * kBlockBytes;

    void fillBlocks(std::vector<uint8_t> &ring, uint32_t firstBlock, uint32_t blocks, int16_t value)
    {
        for (uint32_t at = firstBlock * kBlockBytes; at < (firstBlock + blocks) * kBlockBytes; at += 2u)
        {
            ring[at] = static_cast<uint8_t>(static_cast<uint16_t>(value) & 0xFFu);
            ring[at + 1u] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8u);
        }
    }

    std::unique_ptr<NativeMixer> inputOnly()
    {
        auto mixer = std::make_unique<NativeMixer>();
        mixer->setMasterVolume(1, 0, 0x3FFFu);
        mixer->setMasterVolume(1, 1, 0x3FFFu);
        return mixer;
    }

    bool near(int16_t played, int16_t value)
    {
        return std::abs(played - value) <= std::abs(value) / 50 + 1;
    }
}

void register_native_mixer_tests()
{
    MiniTest::Case("NativeMixer input", [](TestCase &tc)
                   {
        tc.Run("a half is signalled once its last block is taken, a block before it plays", [](TestCase &t)
               {
            std::vector<uint8_t> ring(kRingBytes, 0u);
            auto mixer = inputOnly();
            std::vector<std::pair<uint64_t, int>> halves;
            uint64_t frame = 0;
            mixer->setInputHalfCallback([&](int group, int half)
                                        {
                if (group == 1)
                    halves.emplace_back(frame, half); });
            mixer->startInput(1, ring.data(), kRingBytes, 0u, true);
            std::vector<int16_t> out(2u);
            for (; frame < 2048u; ++frame)
            {
                mixer->render(out.data(), 1u);
                if (frame == 0u)
                    t.Equals(mixer->inputOffset(1), 0x800u, "the first half taken at the start");
            }
            const std::vector<std::pair<uint64_t, int>> expected = {{0u, 0}, {512u, 1}, {1024u, 0}, {1536u, 1}};
            t.Equals(halves.size(), expected.size(), "a signal a half");
            for (size_t i = 0; i < std::min(halves.size(), expected.size()); ++i)
            {
                t.Equals(halves[i].first, expected[i].first, "signalled as the half's last block is taken");
                t.Equals(halves[i].second, expected[i].second, "the half taken");
            }
        });

        tc.Run("a half refilled at its signal plays on the next lap, not before", [](TestCase &t)
               {
            std::vector<uint8_t> ring(kRingBytes, 0u);
            fillBlocks(ring, 0u, 4u, 1000);
            auto mixer = inputOnly();
            int signals = 0;
            mixer->setInputHalfCallback([&](int, int half)
                                        {
                if (half == 0 && ++signals == 2)
                    fillBlocks(ring, 0u, 2u, 2000); });
            mixer->startInput(1, ring.data(), kRingBytes, 0u, true);
            std::vector<int16_t> out(3072u * 2u);
            mixer->render(out.data(), 3072u);
            bool before = true, after = true;
            for (size_t frame = 1024u; frame < 1536u; ++frame)
                before = before && near(out[frame * 2u], 1000) && near(out[frame * 2u + 1u], 1000);
            for (size_t frame = 2048u; frame < 2560u; ++frame)
                after = after && near(out[frame * 2u], 2000) && near(out[frame * 2u + 1u], 2000);
            t.IsTrue(before, "the half taken before the refill plays as it was");
            t.IsTrue(after, "the refill plays on the next lap");
        });

        tc.Run("mixing on the IOP clock signals the halves as it mixes, render only takes", [](TestCase &t)
               {
            std::vector<uint8_t> ring(kRingBytes, 0u);
            fillBlocks(ring, 0u, 4u, 1000);
            auto mixer = inputOnly();
            int signals = 0;
            mixer->setInputHalfCallback([&](int, int) { ++signals; });
            mixer->startInput(1, ring.data(), kRingBytes, 0u, true);
            constexpr uint64_t kCyclesPerSample = 768u;
            for (uint64_t cycle = 0; cycle <= 1000u * kCyclesPerSample; cycle += 16u * kCyclesPerSample)
                mixer->advance(cycle);
            const int mixed = signals;
            t.Equals(mixed, 2, "two halves taken in the first 1000 samples");
            std::vector<int16_t> out(1000u * 2u);
            mixer->render(out.data(), 1000u);
            t.Equals(signals, mixed, "render takes what was mixed and signals nothing");
            t.IsTrue(near(out[0], 1000) && near(out[999u * 2u], 1000), "what was mixed plays");
        });
    });
}
