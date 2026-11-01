#pragma once

// Native sound playback for ports whose sound drivers are taken over at their API (libsd: native_libsd.cpp): the
// game's sample banks (PS2 ADPCM, as its drivers upload them) played by 48 voices (two groups of 24, as the drivers
// address them) with their pitch, ADSR envelope and volume, mixed at 48 kHz for the PC's audio device. There is no
// sound chip here: no registers, no DMA, no interrupts, no chip timing. What a driver sets is a voice's playback
// state, what it reads back (a voice's envelope, which voices reached their end) is that state as it plays.
//
// Reverb: each group's effect send through libsd's reverb presets (room ... pipe, echo and delay with their delay
// time and feedback), the reverb's delay lines in sound memory below the effect end address as the drivers place
// them, at 24 kHz as on the console, mixed back at the group's effect volume.
//
// The sound data input: a driver's PCM stream (sceSdBlockTrans) played from its buffer in IOP memory, 16-bit stereo at
// 48 kHz in blocks of 256 left samples then 256 right, looping or once, with a signal at each half for the driver to
// refill the other. Each group's mix (MMIX) routes the voices, the input and (group 1) group 0's output to the output
// and the reverb.
//
// Not done yet: noise, pitch modulation.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace ps2x::audio
{
    class NativeMixer
    {
    public:
        static constexpr uint32_t kSoundMemory = 2u * 1024u * 1024u; // bytes
        static constexpr uint32_t kSampleRate = 48000u;
        static constexpr int kGroups = 2, kVoices = 24;

        static NativeMixer &instance();
        NativeMixer() { m_groups[1].mix = 0xFFCu; } // sceSdInit's mixes: group 1 also takes group 0's output (dry)

        void reset();
        // The drivers' uploads: bytes into sound memory at address.
        void write(uint32_t address, const uint8_t *data, uint32_t bytes);
        void read(uint32_t address, uint8_t *data, uint32_t bytes) const;

        // A voice's playback settings, as libsd's voice parameters give them (volume: fixed when bit 15 is clear,
        // value << 1 the level; pitch: 0x1000 plays a sample per 48 kHz output sample; ADSR: the PS2's two words).
        void setVolume(int group, int voice, int side, uint16_t value);
        void setPitch(int group, int voice, uint16_t pitch);
        void setAdsr(int group, int voice, int word, uint16_t value);
        void setStart(int group, int voice, uint32_t address);
        void setLoop(int group, int voice, uint32_t address);
        uint16_t volume(int group, int voice, int side) const;
        uint16_t pitch(int group, int voice) const;
        uint16_t adsr(int group, int voice, int word) const;
        uint16_t envelope(int group, int voice) const; // the envelope's level now (0..0x7FFF)
        uint32_t start(int group, int voice) const;
        uint32_t loop(int group, int voice) const;
        uint32_t position(int group, int voice) const; // the block playing now

        // Key on / off (masks of the group's 24 voices); end: voices that reached a block with the end flag since
        // their key on (the drivers poll it to free voices).
        void keyOn(int group, uint32_t mask);
        void keyOff(int group, uint32_t mask);
        uint32_t ended(int group) const;
        void clearEnded(int group, uint32_t mask);
        // Which voices go to the output (left / right) and to the effect send (kept for reverb).
        void setDryMask(int group, int side, uint32_t mask);
        void setWetMask(int group, int side, uint32_t mask);
        uint32_t dryMask(int group, int side) const;
        uint32_t wetMask(int group, int side) const;
        // A group's master and effect volumes (the volume encoding above).
        void setMasterVolume(int group, int side, uint16_t value);
        void setEffectVolume(int group, int side, uint16_t value);
        uint16_t masterVolume(int group, int side) const;
        uint16_t effectVolume(int group, int side) const;

        // A group's reverb: mode 0 (off) .. 9 (pipe) as libsd numbers them; delay and feedback (0..127) for the echo
        // and delay modes; end, the effect area's last byte in sound memory (the area is the mode's size below it).
        void setEffect(int group, int mode, uint32_t delay, uint32_t feedback, uint32_t end);
        void setEffectEnable(int group, bool enable);
        // Zeroes the area mode's reverb uses below end.
        void clearEffectArea(int mode, uint32_t end);
        static uint32_t effectAreaBytes(int mode);

        // A group's mix switches (MMIX: bits 0-3 group 0's output into group 1, 4-7 the input, 8-11 the voices; wet
        // right, wet left, dry right, dry left) and its input volume (16-bit level).
        void setMix(int group, uint16_t mix);
        uint16_t mix(int group) const;
        void setInputVolume(int group, int side, uint16_t value);
        uint16_t inputVolume(int group, int side) const;
        // The input: bytes at memory (a buffer in IOP RAM, which the driver refills while it plays) from offset; loop
        // or once. half(group, half) is called (on the mixer's thread) each time a half has been taken, half 0 or 1:
        // a block (256 samples) before it has played. inputOffset is where the next block is taken from.
        void startInput(int group, const uint8_t *memory, uint32_t bytes, uint32_t offset, bool loop);
        void stopInput(int group);
        bool inputPlaying(int group) const;
        uint32_t inputOffset(int group) const; // the byte the input plays next
        void setInputHalfCallback(std::function<void(int group, int half)> half);

        // The PC's audio device: frames stereo 16-bit samples. Every 10 s of output (PS2X_NATIVE_AUDIO_STATS=0: never),
        // a line on stderr: key ons, bytes uploaded, input halves played, the loudest sample, voices playing, and
        // what reached the SPU2 emulation (absent: nothing).
        void render(int16_t *out, size_t frames);

        // Mixing on the IOP's clock (36.864 MHz, 768 cycles a sample), into a queue render takes from: a driver
        // streaming through a small input ring (two halves of 256 samples) gets each half's signal and refills it in
        // its own time before the mixer reaches it again, whatever the audio device's period. advance(cycle) mixes
        // a sample per step due by then (the IOP calls it at nextEventCycle at the latest), staying within 512
        // samples of the IOP clock; the step follows the queue's level, so the device neither runs dry nor falls
        // behind, and render mixes what is missing while that keeps within the 512. Until the first advance, render
        // mixes itself.
        void advance(uint64_t cycle);
        uint64_t nextEventCycle() const noexcept { return m_nextEvent.load(std::memory_order_relaxed); }

    private:
        struct Volume
        {
            uint16_t setting = 0;
            int32_t level = 0;
            uint32_t counter = 0;
            void set(uint16_t value);
            void tick();
        };
        struct Voice
        {
            Volume volume[2];
            uint16_t pitch = 0;
            uint16_t adsr[2] = {};
            uint32_t start = 0, loop = 0, block = 0;
            bool loopSet = false;
            enum class Phase : uint8_t
            {
                Off,
                Attack,
                Decay,
                Sustain,
                Release
            } phase = Phase::Off;
            int32_t level = 0;
            uint32_t envelopeCounter = 0;
            uint32_t counter = 0;      // 12-bit fraction of the position in the decoded block
            uint8_t flags = 0;         // the block's flags (1 end, 2 repeat, 4 loop start)
            int16_t samples[31] = {};  // 3 of the previous block, then the 28 of this one
            int32_t history[2] = {};
        };
        struct Reverb
        {
            int mode = 0;
            bool enabled = false;
            uint16_t settings[32] = {}; // the preset's: dAPF1, dAPF2, vIIR ... mRAPF2, vLIN, vRIN (offsets in 8 bytes)
            uint32_t start = 0, size = 0;  // the delay lines: first sample (16-bit units) and how many
            uint32_t position = 0;
            bool phase = false;            // the reverb steps every other output sample
            int32_t input[2] = {}, out[2] = {}, previous[2] = {};
        };
        // The input takes its buffer a block (256 samples) ahead of what it plays, as libsd's transfer does: a half
        // has been taken, and is the driver's to refill, once its last block is (the half's signal).
        struct Input
        {
            const uint8_t *memory = nullptr;
            uint32_t samples = 0, position = 0; // in stereo samples
            uint32_t taken = 0;                 // where the next block is taken from
            bool playing = false, loop = false, primed = false;
            int slot = 0;
            int16_t blocks[2][512] = {};        // the block playing and the one taken after it: 256 left, 256 right
            int16_t volume[2] = {0x7FFF, 0x7FFF};
        };
        void takeInputBlock(int group, Input &input, int slot, uint32_t start);
        struct Group
        {
            Voice voices[kVoices];
            uint32_t ended = 0;
            uint32_t dry[2] = {}, wet[2] = {};
            Volume master[2], effect[2];
            Reverb reverb;
            uint16_t mix = 0xFF0u;
            Input input;
        };

        void decode(Voice &voice);
        void stepEnvelope(Voice &voice);
        int32_t sample(Group &group, int index, Voice &voice);
        void stepReverb(Reverb &reverb, const int32_t send[2], int32_t out[2]);

        struct Stats
        {
            uint64_t frames = 0, keyOns = 0, uploaded = 0, halves = 0, gapFrames = 0, droppedFrames = 0;
            int32_t peak = 0;
        } m_stats;
        void report();
        void mix(int16_t *out, size_t frames); // under m_mutex

        mutable std::mutex m_mutex;
        std::vector<uint8_t> m_memory = std::vector<uint8_t>(kSoundMemory, 0u);
        Group m_groups[kGroups];
        std::function<void(int group, int half)> m_inputHalf;

        static constexpr uint64_t kCyclesPerSample = 768u;
        bool m_clocked = false;
        uint64_t m_cycle = 0, m_nextCycle = 0, m_cyclesPerSample = kCyclesPerSample, m_mixed = 0;
        size_t m_pullFrames = 0;
        std::vector<int16_t> m_queue;
        int64_t lead() const noexcept;
        void mixQueued();
        std::atomic<uint64_t> m_nextEvent{0};
    };
}
