// libsd taken over at its API (PS2X_NATIVE_AUDIO=1): the sound drivers a game runs on the IOP (its sequencer and
// synth modules, sdrdrv's remote calls from the EE) keep calling libsd, and each call acts on the native mixer
// (native_mixer.h) instead of the SPU2: a voice parameter, address or switch is that voice's playback state, an upload
// (sceSdVoiceTrans) a copy into the mixer's sound memory, done when the call returns. The SPU2 is not involved.
//
// libsd's numbering (its export table: 4 sceSdInit, 5 sceSdSetParam ... 27 sceSdSetSpu2IntrHandler) and its entries:
// a parameter, switch or address id | voice << 1 | core.

#include "ps2x/iop/iop_native.h"
#include "ps2x/iop/native_mixer.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace
{
    using ps2x::audio::NativeMixer;
    using ps2x::iop::native::Env;

    NativeMixer &mixer()
    {
        return NativeMixer::instance();
    }

    // Voice parameters.
    constexpr uint32_t kVolL = 0x0000u, kVolR = 0x0100u, kPitch = 0x0200u, kAdsr1 = 0x0300u, kAdsr2 = 0x0400u, kEnvX = 0x0500u,
                       kVolXL = 0x0600u, kVolXR = 0x0700u;
    // Core parameters (bit 7 set).
    constexpr uint32_t kMVolL = 0x0980u, kMVolR = 0x0A80u, kEVolL = 0x0B80u, kEVolR = 0x0C80u;
    // Switches.
    constexpr uint32_t kKon = 0x1500u, kKoff = 0x1600u, kEndX = 0x1700u, kVMixL = 0x1800u, kVMixEL = 0x1900u, kVMixR = 0x1A00u,
                       kVMixER = 0x1B00u;
    // Addresses.
    constexpr uint32_t kEea = 0x1D00u, kTsa = 0x1E00u, kSsa = 0x2040u, kLsax = 0x2140u, kNax = 0x2240u;
    // Core attributes.
    constexpr uint32_t kEffectEnable = 0x2u;

    struct State
    {
        std::unordered_map<uint32_t, uint32_t> params, switches, addresses, coreAttributes; // the rest, kept as set
        uint32_t transferAddress[2] = {};
        uint32_t transCallback[2] = {}, irqCallback = 0;
        uint32_t effectEnd[2] = {0x1DFFFFu, 0x1FFFFFu}; // sceSdInit's effect end addresses
        struct Effect
        {
            uint32_t mode = 0, delay = 0, feedback = 0;
        } effect[2];
    } g_state;

    uint32_t arg(Env &e, int n)
    {
        return n < 4 ? e.cpu.gpr[4 + n] : e.r32(e.cpu.gpr[29] + 0x10u + static_cast<uint32_t>(n - 4) * 4u);
    }
    void ret(Env &e, uint32_t value)
    {
        e.cpu.gpr[2] = value;
        e.cpu.pc = e.cpu.gpr[31];
    }

    int coreOf(uint32_t entry)
    {
        return static_cast<int>(entry & 1u);
    }
    int voiceOf(uint32_t entry)
    {
        return static_cast<int>((entry >> 1u) & 0x1Fu) % NativeMixer::kVoices;
    }

    void logCall(const char *what, uint32_t entry, uint32_t value);
    void setParam(uint32_t entry, uint32_t value)
    {
        logCall("param", entry, value);
        const int core = coreOf(entry), voice = voiceOf(entry);
        const uint16_t v = static_cast<uint16_t>(value);
        if ((entry & 0x80u) == 0u)
        {
            switch (entry & 0xFF00u)
            {
            case kVolL: mixer().setVolume(core, voice, 0, v); return;
            case kVolR: mixer().setVolume(core, voice, 1, v); return;
            case kPitch: mixer().setPitch(core, voice, v); return;
            case kAdsr1: mixer().setAdsr(core, voice, 0, v); return;
            case kAdsr2: mixer().setAdsr(core, voice, 1, v); return;
            default: break;
            }
        }
        switch (entry & 0xFFFEu)
        {
        case kMVolL: mixer().setMasterVolume(core, 0, v); return;
        case kMVolR: mixer().setMasterVolume(core, 1, v); return;
        case kEVolL: mixer().setEffectVolume(core, 0, v); return;
        case kEVolR: mixer().setEffectVolume(core, 1, v); return;
        default: g_state.params[entry] = value; return;
        }
    }

    uint32_t getParam(uint32_t entry)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        if ((entry & 0x80u) == 0u)
        {
            switch (entry & 0xFF00u)
            {
            case kVolL: return mixer().volume(core, voice, 0);
            case kVolR: return mixer().volume(core, voice, 1);
            case kPitch: return mixer().pitch(core, voice);
            case kAdsr1: return mixer().adsr(core, voice, 0);
            case kAdsr2: return mixer().adsr(core, voice, 1);
            case kEnvX: return mixer().envelope(core, voice);
            case kVolXL: return mixer().volume(core, voice, 0) & 0x7FFFu;
            case kVolXR: return mixer().volume(core, voice, 1) & 0x7FFFu;
            default: break;
            }
        }
        switch (entry & 0xFFFEu)
        {
        case kMVolL: return mixer().masterVolume(core, 0);
        case kMVolR: return mixer().masterVolume(core, 1);
        case kEVolL: return mixer().effectVolume(core, 0);
        case kEVolR: return mixer().effectVolume(core, 1);
        default:
        {
            const auto it = g_state.params.find(entry);
            return it == g_state.params.end() ? 0u : it->second;
        }
        }
    }

    // PS2X_NATIVE_AUDIO_LOG=1 (diagnosis): the first switch and parameter calls, then counts every 5 s.
    bool logging()
    {
        static const bool s_log = [] { const char *v = std::getenv("PS2X_NATIVE_AUDIO_LOG"); return v && *v == '1'; }();
        return s_log;
    }
    void logCall(const char *what, uint32_t entry, uint32_t value)
    {
        static int s_logged = 0;
        if (logging() && s_logged++ < 200)
            std::fprintf(stderr, "[audio] %s %04x = %x\n", what, entry, value);
    }

    void setSwitch(uint32_t entry, uint32_t value)
    {
        logCall("switch", entry, value);
        const int core = coreOf(entry);
        switch (entry & 0xFF00u)
        {
        case kKon: mixer().keyOn(core, value & 0xFFFFFFu); return;
        case kKoff: mixer().keyOff(core, value & 0xFFFFFFu); return;
        case kEndX: mixer().clearEnded(core, 0xFFFFFFu); return;
        case kVMixL: mixer().setDryMask(core, 0, value); return;
        case kVMixR: mixer().setDryMask(core, 1, value); return;
        case kVMixEL: mixer().setWetMask(core, 0, value); return;
        case kVMixER: mixer().setWetMask(core, 1, value); return;
        default: g_state.switches[entry & 0xFF01u] = value; return;
        }
    }

    uint32_t getSwitch(uint32_t entry)
    {
        const int core = coreOf(entry);
        switch (entry & 0xFF00u)
        {
        case kEndX: return mixer().ended(core);
        case kVMixL: return mixer().dryMask(core, 0);
        case kVMixR: return mixer().dryMask(core, 1);
        case kVMixEL: return mixer().wetMask(core, 0);
        case kVMixER: return mixer().wetMask(core, 1);
        default:
        {
            const auto it = g_state.switches.find(entry & 0xFF01u);
            return it == g_state.switches.end() ? 0u : it->second;
        }
        }
    }

    void setAddr(uint32_t entry, uint32_t value)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        switch (entry & 0xFFC0u)
        {
        case kSsa: mixer().setStart(core, voice, value); return;
        case kLsax: mixer().setLoop(core, voice, value); return;
        default: break;
        }
        if ((entry & 0xFF00u) == kTsa)
            g_state.transferAddress[core] = value;
        if ((entry & 0xFF00u) == kEea)
            g_state.effectEnd[core] = value | 0x1FFFFu;
        g_state.addresses[entry & 0xFF01u] = value;
    }

    uint32_t getAddr(uint32_t entry)
    {
        const int core = coreOf(entry), voice = voiceOf(entry);
        switch (entry & 0xFFC0u)
        {
        case kSsa: return mixer().start(core, voice);
        case kLsax: return mixer().loop(core, voice);
        case kNax: return mixer().position(core, voice);
        default: break;
        }
        if ((entry & 0xFF00u) == kEea)
            return g_state.effectEnd[core];
        const auto it = g_state.addresses.find(entry & 0xFF01u);
        return it == g_state.addresses.end() ? 0u : it->second;
    }

    void setCoreAttr(uint32_t entry, uint32_t value)
    {
        logCall("core attribute", entry, value);
        if ((entry & ~1u) == kEffectEnable)
            mixer().setEffectEnable(coreOf(entry), value != 0u);
        g_state.coreAttributes[entry] = value;
    }

    // The hooks: libsd's calling convention.
    void sdInit(Env &e)
    {
        mixer().reset();
        g_state = State{};
        ret(e, 0u);
    }
    void sdQuit(Env &e)
    {
        ret(e, 0u);
    }
    void sdSetParam(Env &e)
    {
        setParam(arg(e, 0) & 0xFFFFu, arg(e, 1));
        ret(e, 0u);
    }
    void sdGetParam(Env &e)
    {
        ret(e, getParam(arg(e, 0) & 0xFFFFu));
    }
    void sdSetSwitch(Env &e)
    {
        setSwitch(arg(e, 0) & 0xFFFFu, arg(e, 1));
        ret(e, 0u);
    }
    void sdGetSwitch(Env &e)
    {
        ret(e, getSwitch(arg(e, 0) & 0xFFFFu));
    }
    void sdSetAddr(Env &e)
    {
        setAddr(arg(e, 0) & 0xFFFFu, arg(e, 1));
        ret(e, 0u);
    }
    void sdGetAddr(Env &e)
    {
        ret(e, getAddr(arg(e, 0) & 0xFFFFu));
    }
    void sdSetCoreAttr(Env &e)
    {
        setCoreAttr(arg(e, 0) & 0xFFFFu, arg(e, 1));
        ret(e, 0u);
    }
    void sdGetCoreAttr(Env &e)
    {
        const auto it = g_state.coreAttributes.find(arg(e, 0) & 0xFFFFu);
        ret(e, it == g_state.coreAttributes.end() ? 0u : it->second);
    }

    // sceSdProcBatch(SdBatch *batch, u32 *returns, u32 count): batch entries of 8 bytes, {u16 function, u16 entry,
    // u32 value}. sceSdProcBatchEx(batch, returns, count, voices): each entry for every voice of the mask.
    uint32_t batchOne(Env &e, uint32_t function, uint32_t entry, uint32_t value)
    {
        switch (function)
        {
        case 0x01: setParam(entry, value); return 0u;
        case 0x02: setSwitch(entry, value); return 0u;
        case 0x03: setAddr(entry, value); return 0u;
        case 0x04: setCoreAttr(entry, value); return 0u;
        case 0x05: e.w32(entry, value); return 0u; // write to IOP memory
        case 0x10: return getParam(entry);
        case 0x12: return getSwitch(entry);
        case 0x13: return getAddr(entry);
        case 0x14:
        {
            const auto it = g_state.coreAttributes.find(entry);
            return it == g_state.coreAttributes.end() ? 0u : it->second;
        }
        default: return 0u;
        }
    }
    void sdProcBatch(Env &e)
    {
        const uint32_t batch = arg(e, 0), returns = arg(e, 1), count = arg(e, 2);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t at = batch + i * 8u;
            const uint32_t result = batchOne(e, e.r16(at), e.r16(at + 2u), e.r32(at + 4u));
            if (returns != 0u)
                e.w32(returns + i * 4u, result);
        }
        ret(e, count);
    }
    void sdProcBatchEx(Env &e)
    {
        const uint32_t batch = arg(e, 0), returns = arg(e, 1), count = arg(e, 2), voices = arg(e, 3);
        uint32_t written = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t at = batch + i * 8u;
            const uint32_t function = e.r16(at), entry = e.r16(at + 2u), value = e.r32(at + 4u);
            for (uint32_t voice = 0; voice < 24u; ++voice)
            {
                if ((voices & (1u << voice)) == 0u)
                    continue;
                const uint32_t result = batchOne(e, function, (entry & ~0x3Eu) | (voice << 1u), value);
                if (returns != 0u)
                    e.w32(returns + written * 4u, result);
                ++written;
            }
        }
        ret(e, written);
    }

    // sceSdVoiceTrans(channel, mode, iop address, sound memory address, size): mode 0 writes, 1 reads, 2 stops (bits
    // 3+ choose DMA or I/O: the same here). Done when it returns.
    void sdVoiceTrans(Env &e)
    {
        const uint32_t mode = arg(e, 1) & 3u, iop = arg(e, 2), sound = arg(e, 3), size = arg(e, 4);
        if (mode == 0u || mode == 3u)
        {
            std::vector<uint8_t> data(size);
            for (uint32_t i = 0; i < size; ++i)
                data[i] = e.r8(iop + i);
            mixer().write(sound, data.data(), size);
        }
        else if (mode == 1u)
        {
            std::vector<uint8_t> data(size);
            mixer().read(sound, data.data(), size);
            for (uint32_t i = 0; i < size; ++i)
                e.w8(iop + i, data[i]);
        }
        ret(e, mode == 2u ? 0u : size);
    }
    void sdVoiceTransStatus(Env &e)
    {
        ret(e, 1u); // done
    }
    // Block transfers (the sound data input, a PCM stream into the mix) are not rendered yet: accepted, done.
    void sdBlockTrans(Env &e)
    {
        ret(e, 0u);
    }
    void sdBlockTransStatus(Env &e)
    {
        ret(e, 0u);
    }
    void sdSetTransCallback(Env &e)
    {
        const uint32_t channel = arg(e, 0) & 1u, previous = g_state.transCallback[channel];
        g_state.transCallback[channel] = arg(e, 1);
        if (arg(e, 1) != 0u)
            std::fprintf(stderr, "[audio] libsd transfer callback %08x on channel %u: not called (transfers end at once)\n", arg(e, 1), channel);
        ret(e, previous);
    }
    void sdSetIrqCallback(Env &e)
    {
        const uint32_t previous = g_state.irqCallback;
        g_state.irqCallback = arg(e, 0);
        ret(e, previous);
    }
    void sdReturnZero(Env &e)
    {
        ret(e, 0u);
    }

    // sceSdSetEffectAttr(core, attr): attr {int core; int mode (bit 8: clear the area first); short depth_L, depth_R;
    // int delay; int feedback}. The depths are the effect volumes; the area is the mode's size below the effect end.
    void sdSetEffectAttr(Env &e)
    {
        const int core = static_cast<int>(arg(e, 0) & 1u);
        const uint32_t attr = arg(e, 1);
        const uint32_t mode = e.r32(attr + 4u);
        if ((mode & 0xFFu) >= 10u)
        {
            ret(e, 0xFFFFFFFFu);
            return;
        }
        auto &effect = g_state.effect[core];
        effect.mode = mode & 0xFFu;
        effect.delay = e.r32(attr + 0xCu);
        effect.feedback = e.r32(attr + 0x10u);
        if (logging())
            std::fprintf(stderr, "[audio] effect core %d mode %x depth %x/%x delay %u feedback %u end %x\n", core, mode,
                         e.r16(attr + 8u), e.r16(attr + 0xAu), effect.delay, effect.feedback, g_state.effectEnd[core]);
        if ((mode & 0x100u) != 0u)
            mixer().clearEffectArea(static_cast<int>(effect.mode), g_state.effectEnd[core]);
        mixer().setEffect(core, static_cast<int>(effect.mode), effect.delay, effect.feedback, g_state.effectEnd[core]);
        mixer().setEffectVolume(core, 0, e.r16(attr + 8u));
        mixer().setEffectVolume(core, 1, e.r16(attr + 0xAu));
        ret(e, 0u);
    }
    void sdGetEffectAttr(Env &e)
    {
        const int core = static_cast<int>(arg(e, 0) & 1u);
        const uint32_t attr = arg(e, 1);
        const auto &effect = g_state.effect[core];
        e.w32(attr + 4u, effect.mode);
        e.w16(attr + 8u, mixer().effectVolume(core, 0));
        e.w16(attr + 0xAu, mixer().effectVolume(core, 1));
        e.w32(attr + 0xCu, effect.delay);
        e.w32(attr + 0x10u, effect.feedback);
        ret(e, 0u);
    }
    // sceSdClearEffectWorkArea(core, channel, mode).
    void sdClearEffectWorkArea(Env &e)
    {
        const int core = static_cast<int>(arg(e, 0) & 1u);
        const uint32_t mode = arg(e, 2) & 0xFFu;
        if (mode < 10u)
            mixer().clearEffectArea(static_cast<int>(mode), g_state.effectEnd[core]);
        ret(e, 0u);
    }

    // PS2X_NATIVE_AUDIO=1: libsd's exports taken over (static initialization, before any module loads).
    const bool g_registered = []
    {
        const char *value = std::getenv("PS2X_NATIVE_AUDIO");
        if (!(value && *value == '1'))
            return false;
        using ps2x::iop::native::overrideExport;
        overrideExport("libsd", 2u, &sdQuit);
        overrideExport("libsd", 4u, &sdInit);
        overrideExport("libsd", 5u, &sdSetParam);
        overrideExport("libsd", 6u, &sdGetParam);
        overrideExport("libsd", 7u, &sdSetSwitch);
        overrideExport("libsd", 8u, &sdGetSwitch);
        overrideExport("libsd", 9u, &sdSetAddr);
        overrideExport("libsd", 10u, &sdGetAddr);
        overrideExport("libsd", 11u, &sdSetCoreAttr);
        overrideExport("libsd", 12u, &sdGetCoreAttr);
        overrideExport("libsd", 15u, &sdProcBatch);
        overrideExport("libsd", 16u, &sdProcBatchEx);
        overrideExport("libsd", 17u, &sdVoiceTrans);
        overrideExport("libsd", 18u, &sdBlockTrans);
        overrideExport("libsd", 19u, &sdVoiceTransStatus);
        overrideExport("libsd", 20u, &sdBlockTransStatus);
        overrideExport("libsd", 21u, &sdSetTransCallback);
        overrideExport("libsd", 22u, &sdSetIrqCallback);
        overrideExport("libsd", 23u, &sdSetEffectAttr);
        overrideExport("libsd", 24u, &sdGetEffectAttr);
        overrideExport("libsd", 25u, &sdClearEffectWorkArea);
        overrideExport("libsd", 26u, &sdReturnZero); // sceSdSetTransIntrHandler
        overrideExport("libsd", 27u, &sdReturnZero); // sceSdSetSpu2IntrHandler
        return true;
    }();
}

namespace ps2x::audio
{
    bool nativeLibsd()
    {
        return g_registered;
    }
}
