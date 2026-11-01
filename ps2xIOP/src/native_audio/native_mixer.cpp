#include "ps2x/iop/native_mixer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ps2x::audio
{
    namespace
    {
        // PS2 ADPCM: a 16-byte block is a header (shift, filter, flags) and 28 4-bit samples; each filter predicts
        // from the two samples before.
        constexpr int32_t kFilterPositive[5] = {0, 60, 115, 98, 122};
        constexpr int32_t kFilterNegative[5] = {0, 0, -52, -55, -60};

        int32_t clamp16(int32_t value)
        {
            return std::clamp(value, -0x8000, 0x7FFF);
        }

        int32_t mul15(int32_t a, int32_t b)
        {
            return (a * b) >> 15;
        }

        // One step of an envelope or a volume sweep: rate is (shift << 2) | step, 0..127, at 48 kHz.
        void envelopeStep(int32_t &level, uint32_t &counter, uint32_t rate, bool decreasing, bool exponential)
        {
            const int32_t shift = static_cast<int32_t>(rate >> 2u);
            int32_t step = 7 - static_cast<int32_t>(rate & 3u);
            if (decreasing)
                step = ~step;
            uint32_t increment = 0x8000u;
            if (shift < 11)
                step *= 1 << (11 - shift);
            else if (shift > 11)
                increment = shift - 11 < 16 ? increment >> (shift - 11) : 0u;
            if (exponential)
            {
                if (decreasing)
                    step = (step * level) >> 15;
                else if (level >= 0x6000)
                {
                    if (shift < 10)
                        step >>= 2;
                    else if (shift >= 11)
                        increment >>= 2;
                    else
                    {
                        step >>= 1;
                        increment >>= 1;
                    }
                }
            }
            counter += increment;
            if ((counter & 0x8000u) == 0u)
                return;
            counter = 0u;
            level = std::clamp(level + step, 0, 0x7FFF);
        }

        // 4-point Catmull-Rom between y1 and y2; t a 12-bit fraction.
        int32_t interpolate(int32_t y0, int32_t y1, int32_t y2, int32_t y3, uint32_t t)
        {
            const float f = static_cast<float>(t) * (1.0f / 4096.0f);
            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = static_cast<float>(y0) - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c = -0.5f * y0 + 0.5f * y2;
            return clamp16(static_cast<int32_t>(((a * f + b) * f + c) * f + static_cast<float>(y1)));
        }
    }

    void NativeMixer::Volume::set(uint16_t value)
    {
        setting = value;
        counter = 0u;
        if ((value & 0x8000u) == 0u)
            level = static_cast<int16_t>(static_cast<uint16_t>(value << 1u));
    }

    void NativeMixer::Volume::tick()
    {
        if ((setting & 0x8000u) == 0u)
            return;
        const bool negative = (setting & 0x1000u) != 0u;
        int32_t magnitude = std::abs(level);
        envelopeStep(magnitude, counter, setting & 0x7Fu, (setting & 0x2000u) != 0u, (setting & 0x4000u) != 0u);
        level = negative ? -magnitude : magnitude;
    }

    NativeMixer &NativeMixer::instance()
    {
        static NativeMixer mixer;
        return mixer;
    }

    void NativeMixer::reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Group &group : m_groups)
            group = Group{};
    }

    void NativeMixer::write(uint32_t address, const uint8_t *data, uint32_t bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (uint32_t i = 0; i < bytes; ++i)
            m_memory[(address + i) % kSoundMemory] = data[i];
    }

    void NativeMixer::read(uint32_t address, uint8_t *data, uint32_t bytes) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (uint32_t i = 0; i < bytes; ++i)
            data[i] = m_memory[(address + i) % kSoundMemory];
    }

    void NativeMixer::setVolume(int group, int voice, int side, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].voices[voice].volume[side].set(value);
    }
    void NativeMixer::setPitch(int group, int voice, uint16_t pitch)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].voices[voice].pitch = pitch;
    }
    void NativeMixer::setAdsr(int group, int voice, int word, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].voices[voice].adsr[word] = value;
    }
    void NativeMixer::setStart(int group, int voice, uint32_t address)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].voices[voice].start = address & (kSoundMemory - 1u) & ~15u;
    }
    void NativeMixer::setLoop(int group, int voice, uint32_t address)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Voice &v = m_groups[group].voices[voice];
        v.loop = address & (kSoundMemory - 1u) & ~15u;
        v.loopSet = true;
    }
    uint16_t NativeMixer::volume(int group, int voice, int side) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].voices[voice].volume[side].setting;
    }
    uint16_t NativeMixer::pitch(int group, int voice) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].voices[voice].pitch;
    }
    uint16_t NativeMixer::adsr(int group, int voice, int word) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].voices[voice].adsr[word];
    }
    uint16_t NativeMixer::envelope(int group, int voice) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint16_t>(m_groups[group].voices[voice].level);
    }
    uint32_t NativeMixer::start(int group, int voice) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].voices[voice].start;
    }
    uint32_t NativeMixer::loop(int group, int voice) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].voices[voice].loop;
    }
    uint32_t NativeMixer::position(int group, int voice) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].voices[voice].block;
    }

    void NativeMixer::keyOn(int group, uint32_t mask)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Group &g = m_groups[group];
        for (int index = 0; index < kVoices; ++index)
        {
            if ((mask & (1u << index)) == 0u)
                continue;
            Voice &voice = g.voices[index];
            voice.phase = Voice::Phase::Attack;
            voice.level = 0;
            voice.envelopeCounter = 0u;
            voice.counter = 0u;
            std::fill(std::begin(voice.samples), std::end(voice.samples), static_cast<int16_t>(0));
            voice.history[0] = voice.history[1] = 0;
            voice.block = voice.start;
            voice.loopSet = false;
            g.ended &= ~(1u << index);
            decode(voice);
        }
    }

    void NativeMixer::keyOff(int group, uint32_t mask)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int index = 0; index < kVoices; ++index)
        {
            Voice &voice = m_groups[group].voices[index];
            if ((mask & (1u << index)) == 0u || voice.phase == Voice::Phase::Off)
                continue;
            voice.phase = Voice::Phase::Release;
            voice.envelopeCounter = 0u;
        }
    }

    uint32_t NativeMixer::ended(int group) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].ended;
    }
    void NativeMixer::clearEnded(int group, uint32_t mask)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].ended &= ~mask;
    }
    void NativeMixer::setDryMask(int group, int side, uint32_t mask)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].dry[side] = mask & 0xFFFFFFu;
    }
    void NativeMixer::setWetMask(int group, int side, uint32_t mask)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].wet[side] = mask & 0xFFFFFFu;
    }
    uint32_t NativeMixer::dryMask(int group, int side) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].dry[side];
    }
    uint32_t NativeMixer::wetMask(int group, int side) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].wet[side];
    }
    void NativeMixer::setMasterVolume(int group, int side, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].master[side].set(value);
    }
    void NativeMixer::setEffectVolume(int group, int side, uint16_t value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].effect[side].set(value);
    }
    uint16_t NativeMixer::masterVolume(int group, int side) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].master[side].setting;
    }
    uint16_t NativeMixer::effectVolume(int group, int side) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_groups[group].effect[side].setting;
    }

    void NativeMixer::decode(Voice &voice)
    {
        const uint8_t *block = &m_memory[voice.block & (kSoundMemory - 16u)];
        uint32_t shift = block[0] & 0xFu, filter = (block[0] >> 4u) & 7u;
        if (shift > 12u)
            shift = 9u;
        if (filter > 4u)
            filter = 4u;
        voice.flags = block[1];
        if ((voice.flags & 4u) != 0u && !voice.loopSet)
            voice.loop = voice.block;
        voice.samples[0] = voice.samples[28];
        voice.samples[1] = voice.samples[29];
        voice.samples[2] = voice.samples[30];
        int32_t h0 = voice.history[0], h1 = voice.history[1];
        for (uint32_t i = 0; i < 28u; ++i)
        {
            const uint32_t nibble = (block[2u + (i >> 1u)] >> ((i & 1u) * 4u)) & 0xFu;
            int32_t value = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12u)) >> shift;
            value = clamp16(value + ((h0 * kFilterPositive[filter] + h1 * kFilterNegative[filter] + 32) >> 6));
            h1 = h0;
            h0 = value;
            voice.samples[3u + i] = static_cast<int16_t>(value);
        }
        voice.history[0] = h0;
        voice.history[1] = h1;
    }

    void NativeMixer::stepEnvelope(Voice &voice)
    {
        const uint16_t a1 = voice.adsr[0], a2 = voice.adsr[1];
        switch (voice.phase)
        {
        case Voice::Phase::Attack:
            envelopeStep(voice.level, voice.envelopeCounter, (a1 >> 8u) & 0x7Fu, false, (a1 & 0x8000u) != 0u);
            if (voice.level >= 0x7FFF)
            {
                voice.phase = Voice::Phase::Decay;
                voice.envelopeCounter = 0u;
            }
            break;
        case Voice::Phase::Decay:
            envelopeStep(voice.level, voice.envelopeCounter, ((a1 >> 4u) & 0xFu) << 2u, true, true);
            if (voice.level <= std::min<int32_t>(((a1 & 0xFu) + 1) * 0x800, 0x7FFF))
            {
                voice.phase = Voice::Phase::Sustain;
                voice.envelopeCounter = 0u;
            }
            break;
        case Voice::Phase::Sustain:
            envelopeStep(voice.level, voice.envelopeCounter, (a2 >> 6u) & 0x7Fu, (a2 & 0x4000u) != 0u, (a2 & 0x8000u) != 0u);
            break;
        case Voice::Phase::Release:
            envelopeStep(voice.level, voice.envelopeCounter, (a2 & 0x1Fu) << 2u, true, (a2 & 0x20u) != 0u);
            if (voice.level <= 0)
            {
                voice.level = 0;
                voice.phase = Voice::Phase::Off;
            }
            break;
        case Voice::Phase::Off:
            break;
        }
    }

    int32_t NativeMixer::sample(Group &group, int index, Voice &voice)
    {
        const uint32_t at = voice.counter >> 12u;
        int32_t value = interpolate(voice.samples[at], voice.samples[at + 1u], voice.samples[at + 2u], voice.samples[at + 3u],
                                    voice.counter & 0xFFFu);
        stepEnvelope(voice);
        value = mul15(value, voice.level);
        voice.counter += std::min<uint32_t>(voice.pitch, 0x3FFFu);
        while ((voice.counter >> 12u) >= 28u)
        {
            voice.counter -= 28u << 12u;
            if ((voice.flags & 1u) != 0u)
            {
                // The block ends the sample: on to the loop point; without the repeat flag the voice stops.
                group.ended |= 1u << index;
                voice.block = voice.loop;
                if ((voice.flags & 2u) == 0u)
                {
                    voice.phase = Voice::Phase::Off;
                    voice.level = 0;
                }
            }
            else
                voice.block = (voice.block + 16u) & (kSoundMemory - 1u);
            decode(voice);
        }
        return value;
    }

    void NativeMixer::render(int16_t *out, size_t frames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (size_t frame = 0; frame < frames; ++frame)
        {
            // Group 0's output goes through group 1's master volume too, as the drivers set them up.
            int32_t mixed[2] = {0, 0};
            for (int g = 0; g < kGroups; ++g)
            {
                Group &group = m_groups[g];
                int32_t dry[2] = {0, 0};
                for (int index = 0; index < kVoices; ++index)
                {
                    Voice &voice = group.voices[index];
                    voice.volume[0].tick();
                    voice.volume[1].tick();
                    if (voice.phase == Voice::Phase::Off)
                        continue;
                    const int32_t value = sample(group, index, voice);
                    const uint32_t bit = 1u << index;
                    if ((group.dry[0] & bit) != 0u)
                        dry[0] += mul15(value, voice.volume[0].level);
                    if ((group.dry[1] & bit) != 0u)
                        dry[1] += mul15(value, voice.volume[1].level);
                }
                group.master[0].tick();
                group.master[1].tick();
                for (int side = 0; side < 2; ++side)
                {
                    const int32_t level = clamp16(dry[side]);
                    mixed[side] = g == 0 ? mul15(level, group.master[side].level) : clamp16(mixed[side] + level);
                }
                if (g == kGroups - 1)
                    for (int side = 0; side < 2; ++side)
                        mixed[side] = mul15(mixed[side], group.master[side].level);
            }
            out[frame * 2u] = static_cast<int16_t>(clamp16(mixed[0]));
            out[frame * 2u + 1u] = static_cast<int16_t>(clamp16(mixed[1]));
        }
    }
}
