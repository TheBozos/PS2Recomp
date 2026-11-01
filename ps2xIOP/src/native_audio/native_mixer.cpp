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

        // libsd's reverb presets (its table, mode 0 off .. 9 pipe): dAPF1, dAPF2, vIIR, vCOMB1..4, vWALL, vAPF1, vAPF2,
        // mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4,
        // mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN; offsets in units of 8 bytes. And each
        // mode's effect area, in the same units.
        constexpr uint16_t kPresets[10][32] = {
            {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
            {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
            {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
            {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
            {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
            {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
            {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
            {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
            {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
            {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
        };
        constexpr uint32_t kAreaSizes[10] = {0x2, 0x4D8, 0x3E8, 0x908, 0xDFC, 0x15BC, 0x1ED8, 0x3008, 0x3008, 0x780};

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

    uint32_t NativeMixer::effectAreaBytes(int mode)
    {
        return kAreaSizes[std::clamp(mode, 0, 9)] * 8u;
    }

    void NativeMixer::setEffect(int group, int mode, uint32_t delay, uint32_t feedback, uint32_t end)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Reverb &reverb = m_groups[group].reverb;
        mode = std::clamp(mode, 0, 9);
        reverb.mode = mode;
        std::copy(std::begin(kPresets[mode]), std::end(kPresets[mode]), reverb.settings);
        if (mode == 7 || mode == 8)
        {
            // Echo and delay: the delay time moves the same-side and all-pass lines (as libsd's sceSdSetEffectAttr
            // does), the feedback is the wall coefficient.
            const uint32_t t = std::min(delay, 127u) + 1u;
            uint16_t *s = reverb.settings;
            s[10] = static_cast<uint16_t>((t << 6u) - s[0]);
            s[11] = static_cast<uint16_t>((t << 5u) - s[1]);
            s[16] = static_cast<uint16_t>(s[17] + (t << 5u));
            s[12] = static_cast<uint16_t>(s[13] + (t << 5u));
            s[27] = static_cast<uint16_t>(s[29] + (t << 5u));
            s[26] = static_cast<uint16_t>(s[28] + (t << 5u));
            s[7] = static_cast<uint16_t>(std::min(feedback, 127u) * 258u);
        }
        // The area: the mode's size below end (its last byte), in 16-bit samples.
        end = (end | 0x1FFFFu) & (kSoundMemory - 1u);
        reverb.size = kAreaSizes[mode] * 4u;
        reverb.start = ((end + 1u) / 2u - reverb.size) & (kSoundMemory / 2u - 1u);
        reverb.position = 0u;
        reverb.input[0] = reverb.input[1] = reverb.out[0] = reverb.out[1] = reverb.previous[0] = reverb.previous[1] = 0;
    }

    void NativeMixer::setEffectEnable(int group, bool enable)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_groups[group].reverb.enabled = enable;
    }

    void NativeMixer::clearEffectArea(int mode, uint32_t end)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        end = (end | 0x1FFFFu) & (kSoundMemory - 1u);
        const uint32_t bytes = effectAreaBytes(mode);
        for (uint32_t i = 0; i < bytes; ++i)
            m_memory[(end + 1u - bytes + i) & (kSoundMemory - 1u)] = 0u;
    }

    // One step of the console's reverb (24 kHz): the same-side and cross IIR lines, four combs, two all-pass filters,
    // on the delay lines in sound memory, which advance a sample each step.
    void NativeMixer::stepReverb(Reverb &reverb, const int32_t send[2], int32_t out[2])
    {
        const uint32_t size = reverb.size;
        const auto at = [&](uint32_t offset, int32_t delta = 0)
        {
            const uint32_t relative = (reverb.position + offset + static_cast<uint32_t>(delta + static_cast<int32_t>(size))) % size;
            return ((reverb.start + relative) & (kSoundMemory / 2u - 1u)) * 2u;
        };
        const auto load = [&](uint32_t offset, int32_t delta = 0)
        {
            const uint32_t a = at(offset, delta);
            return static_cast<int32_t>(static_cast<int16_t>(m_memory[a] | (m_memory[a + 1u] << 8u)));
        };
        const auto store = [&](uint32_t offset, int32_t value)
        {
            const uint32_t a = at(offset);
            const uint16_t v = static_cast<uint16_t>(clamp16(value));
            m_memory[a] = static_cast<uint8_t>(v);
            m_memory[a + 1u] = static_cast<uint8_t>(v >> 8u);
        };
        const uint16_t *s = reverb.settings;
        const auto gain = [&](int index) { return static_cast<int32_t>(static_cast<int16_t>(s[index])); };
        const auto line = [&](int index) { return static_cast<uint32_t>(s[index]) * 4u; };
        const int32_t vIIR = gain(2), vCOMB1 = gain(3), vCOMB2 = gain(4), vCOMB3 = gain(5), vCOMB4 = gain(6),
                      vWALL = gain(7), vAPF1 = gain(8), vAPF2 = gain(9), vLIN = gain(30), vRIN = gain(31);
        const uint32_t dAPF1 = line(0), dAPF2 = line(1), mLSAME = line(10), mRSAME = line(11), mLCOMB1 = line(12),
                       mRCOMB1 = line(13), mLCOMB2 = line(14), mRCOMB2 = line(15), dLSAME = line(16), dRSAME = line(17),
                       mLDIFF = line(18), mRDIFF = line(19), mLCOMB3 = line(20), mRCOMB3 = line(21), mLCOMB4 = line(22),
                       mRCOMB4 = line(23), dLDIFF = line(24), dRDIFF = line(25), mLAPF1 = line(26), mRAPF1 = line(27),
                       mLAPF2 = line(28), mRAPF2 = line(29);

        const int32_t lin = mul15(clamp16(send[0]), vLIN);
        const int32_t rin = mul15(clamp16(send[1]), vRIN);
        store(mLSAME, mul15(lin + mul15(load(dLSAME), vWALL) - load(mLSAME, -1), vIIR) + load(mLSAME, -1));
        store(mRSAME, mul15(rin + mul15(load(dRSAME), vWALL) - load(mRSAME, -1), vIIR) + load(mRSAME, -1));
        store(mLDIFF, mul15(lin + mul15(load(dRDIFF), vWALL) - load(mLDIFF, -1), vIIR) + load(mLDIFF, -1));
        store(mRDIFF, mul15(rin + mul15(load(dLDIFF), vWALL) - load(mRDIFF, -1), vIIR) + load(mRDIFF, -1));

        int32_t lout = mul15(vCOMB1, load(mLCOMB1)) + mul15(vCOMB2, load(mLCOMB2)) + mul15(vCOMB3, load(mLCOMB3)) +
                       mul15(vCOMB4, load(mLCOMB4));
        int32_t rout = mul15(vCOMB1, load(mRCOMB1)) + mul15(vCOMB2, load(mRCOMB2)) + mul15(vCOMB3, load(mRCOMB3)) +
                       mul15(vCOMB4, load(mRCOMB4));
        const auto allPass = [&](int32_t value, uint32_t destination, uint32_t delay, int32_t coefficient)
        {
            const int32_t delayed = load(destination, -static_cast<int32_t>(delay));
            value = clamp16(value - mul15(coefficient, delayed));
            store(destination, value);
            return mul15(value, coefficient) + delayed;
        };
        lout = allPass(lout, mLAPF1, dAPF1, vAPF1);
        rout = allPass(rout, mRAPF1, dAPF1, vAPF1);
        lout = allPass(lout, mLAPF2, dAPF2, vAPF2);
        rout = allPass(rout, mRAPF2, dAPF2, vAPF2);

        reverb.position = reverb.position + 1u >= size ? 0u : reverb.position + 1u;
        out[0] = clamp16(lout);
        out[1] = clamp16(rout);
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
                int32_t dry[2] = {0, 0}, wet[2] = {0, 0};
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
                    if ((group.wet[0] & bit) != 0u)
                        wet[0] += mul15(value, voice.volume[0].level);
                    if ((group.wet[1] & bit) != 0u)
                        wet[1] += mul15(value, voice.volume[1].level);
                }
                // The reverb: the send averaged over two output samples, a step every other sample, its output
                // eased between steps; at the group's effect volume.
                Reverb &reverb = group.reverb;
                group.effect[0].tick();
                group.effect[1].tick();
                if (reverb.enabled && reverb.mode != 0)
                {
                    const int32_t send[2] = {clamp16(wet[0]), clamp16(wet[1])};
                    if (reverb.phase)
                    {
                        const int32_t average[2] = {(reverb.input[0] + send[0]) >> 1, (reverb.input[1] + send[1]) >> 1};
                        reverb.previous[0] = reverb.out[0];
                        reverb.previous[1] = reverb.out[1];
                        stepReverb(reverb, average, reverb.out);
                    }
                    else
                    {
                        reverb.input[0] = send[0];
                        reverb.input[1] = send[1];
                    }
                    for (int side = 0; side < 2; ++side)
                    {
                        const int32_t value = reverb.phase ? (reverb.previous[side] + reverb.out[side]) >> 1 : reverb.out[side];
                        dry[side] += mul15(value, group.effect[side].level);
                    }
                    reverb.phase = !reverb.phase;
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
