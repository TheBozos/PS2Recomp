// The reference recorder (runtime/ps2_reference.h).

#include "runtime/ps2_reference.h"
#include "runtime/ps2_capture.h"
#include "runtime/ps2_calllog.h"
#include "runtime/ps2_determinism.h"
#include "runtime/ee_scheduler.h"

#include "ps2_runtime.h"
#include "ps2x/iop/native_mixer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::reference
{
    bool g_tracingCalls = false;

    namespace
    {
        constexpr uint64_t kP1 = 0x9E3779B185EBCA87ull, kP2 = 0xC2B2AE3D27D4EB4Full, kP3 = 0x165667B19E3779F9ull,
                           kP4 = 0x85EBCA77C2B2AE63ull, kP5 = 0x27D4EB2F165667C5ull;

        inline uint64_t rotl(uint64_t value, int bits) { return (value << bits) | (value >> (64 - bits)); }
        inline uint64_t read64(const uint8_t *p)
        {
            uint64_t value;
            std::memcpy(&value, p, 8);
            return value;
        }
        inline uint32_t read32(const uint8_t *p)
        {
            uint32_t value;
            std::memcpy(&value, p, 4);
            return value;
        }
        inline uint64_t round(uint64_t acc, uint64_t input)
        {
            acc += input * kP2;
            acc = rotl(acc, 31);
            return acc * kP1;
        }
        inline uint64_t mergeRound(uint64_t acc, uint64_t value)
        {
            acc ^= round(0, value);
            return acc * kP1 + kP4;
        }

        uint32_t fnv1a32(const char *text)
        {
            uint32_t hash = 0x811C9DC5u;
            for (; *text; ++text)
                hash = (hash ^ static_cast<uint8_t>(*text)) * 0x01000193u;
            return hash;
        }

        // The file's layout (little-endian; docs/REFERENCE_HARNESS.md in TMB_Recomp).
        constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'R', 'E', 'F', '1'};
        constexpr uint32_t kVersion = 1u;
        constexpr uint32_t kHashXxh64 = 1u; // ram, scratchpad, calls, draws, threads: XXH64 seed 0; sound: FNV-1a 64
#pragma pack(push, 1)
        struct Header
        {
            char magic[8];
            uint32_t version;
            uint32_t headerBytes; // this struct and the masks after it
            uint32_t recordBytes;
            uint32_t hash;
            uint32_t ramBytes;
            uint32_t scratchpadBytes;
            uint64_t elfHash; // XXH64 of the ELF file
            uint32_t flags;   // bit 0: PS2X_DETERMINISTIC=1
            uint32_t maskCount;
            char run[64];
            char build[64];
            char elf[64]; // the ELF's file name
        };
        struct Record
        {
            uint32_t frame;
            int32_t thread; // the guest thread running at the frame end
            uint64_t ram;
            uint64_t scratchpad;
            uint64_t calls;
            uint64_t draws;
            uint64_t sound;
            uint64_t threads; // XXH64 of int32 {running, ready threads in run order...}
            uint64_t eeCycle;
            uint32_t callCount;
            uint32_t drawBatches;
            uint32_t drawVertices;
            uint32_t soundSamples; // stereo samples mixed during the frame
        };
        struct PackedCall
        {
            uint32_t id, a0, a1, a2, a3, v0, flags;
        };
#pragma pack(pop)
        static_assert(sizeof(Header) == 240, "header layout");
        static_assert(sizeof(Record) == 80, "record layout");
        static_assert(sizeof(PackedCall) == 28, "call layout");

        enum CallFlags : uint32_t
        {
            kBlocked = 1u,       // the call did not return to its caller (a wait; its result comes later)
            kOpenAtFrameEnd = 2u // the frame ended inside the call (a frame end such as sceGsSyncV)
        };

        struct Call
        {
            uint32_t id = 0, pc = 0;
            uint32_t a[4] = {};
            uint32_t v0 = 0, flags = 0;
            bool done = false; // returned or blocked
        };

        struct State
        {
            bool initialized = false;
            bool active = false;
            std::string path;
            std::string run;
            std::string build = std::string(__DATE__) + " " + __TIME__;
            FILE *out = nullptr;
            FILE *callsOut = nullptr;
            uint64_t framesLimit = 0;
            uint64_t callsFrom = 0, callsCount = 120;
            std::vector<std::pair<uint64_t, uint64_t>> dumps; // frame ranges (inclusive)
            std::vector<std::pair<uint32_t, uint32_t>> masks;
            std::vector<uint8_t> masked; // RAM with the masks zeroed (only with masks)
            uint64_t records = 0;
            bool checkHost = false;

            // Calls
            std::unordered_map<uint32_t, uint32_t> nameIndex; // address -> names index
            std::vector<std::string> names;
            std::vector<uint32_t> ids;
            std::vector<uint64_t> traced = std::vector<uint64_t>((PS2_RAM_SIZE / 4u + 63u) / 64u, 0u);
            std::vector<Call> calls;
            uint64_t generation = 0; // frames ended: a call's slot is valid in its own frame only

            // Draws (any thread)
            std::mutex drawMutex;
            uint64_t draws = 0;
            uint32_t drawBatches = 0, drawVertices = 0;

            std::vector<int> ready;
            std::vector<int32_t> threadWords;
            std::vector<PackedCall> packed;
        };

        State &state()
        {
            static State s_state;
            return s_state;
        }

        void parseRanges(const char *text, std::vector<std::pair<uint64_t, uint64_t>> &out)
        {
            while (text && *text)
            {
                char *end = nullptr;
                const uint64_t first = std::strtoull(text, &end, 0);
                uint64_t last = first;
                if (end && *end == '-')
                    last = std::strtoull(end + 1, &end, 0);
                if (end == text)
                    break;
                out.emplace_back(first, std::max(first, last));
                text = end && *end == ',' ? end + 1 : end;
                if (!text || (*text && *text != ',' && !(*text >= '0' && *text <= '9')))
                    break;
            }
        }

        void initialize(State &s)
        {
            s.initialized = true;
            const char *path = std::getenv("PS2X_REFERENCE");
            if (!path || !*path)
                return;
            s.path = path;
            s.active = true;
            g_tracingCalls = true; // raw syscalls from now on; stubs once the port names them (setCallNames)
            if (const char *run = std::getenv("PS2X_REFERENCE_RUN"); run && *run)
                s.run = run;
            else
                s.run = std::filesystem::path(s.path).stem().string();
            if (const char *frames = std::getenv("PS2X_REFERENCE_FRAMES"); frames && *frames)
                s.framesLimit = std::strtoull(frames, nullptr, 10);
            if (const char *calls = std::getenv("PS2X_REFERENCE_CALLS"); calls && *calls)
                s.callsCount = std::strtoull(calls, nullptr, 10);
            if (const char *from = std::getenv("PS2X_REFERENCE_CALLS_FROM"); from && *from)
                s.callsFrom = std::strtoull(from, nullptr, 10);
            parseRanges(std::getenv("PS2X_REFERENCE_DUMP"), s.dumps);
            if (const char *masks = std::getenv("PS2X_REFERENCE_MASK"); masks && *masks)
            {
                for (const char *at = masks; at && *at;)
                {
                    char *end = nullptr;
                    const uint32_t address = static_cast<uint32_t>(std::strtoul(at, &end, 0)) & (PS2_RAM_SIZE - 1u);
                    if (!end || *end != ':')
                        break;
                    const uint32_t bytes = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
                    s.masks.emplace_back(address, std::min(bytes, PS2_RAM_SIZE - address));
                    at = end && *end == ',' ? end + 1 : nullptr;
                }
            }
            s.checkHost = std::getenv("PS2X_REFERENCE_CHECK_HOST") != nullptr;
        }

        void copyName(char *out, size_t size, const std::string &text)
        {
            std::memset(out, 0, size);
            std::memcpy(out, text.data(), std::min(text.size(), size - 1u));
        }

        bool openFile(State &s)
        {
            s.out = std::fopen(s.path.c_str(), "wb");
            if (!s.out)
            {
                std::fprintf(stderr, "[ref] cannot write %s: recording off\n", s.path.c_str());
                s.active = false;
                g_tracingCalls = false;
                return false;
            }
            Header header{};
            std::memcpy(header.magic, kMagic, sizeof(kMagic));
            header.version = kVersion;
            header.headerBytes = static_cast<uint32_t>(sizeof(Header) + s.masks.size() * 8u);
            header.recordBytes = sizeof(Record);
            header.hash = kHashXxh64;
            header.ramBytes = PS2_RAM_SIZE;
            header.scratchpadBytes = PS2_SCRATCHPAD_SIZE;
            const std::filesystem::path elf = PS2Runtime::getIoPaths().elfPath;
            if (FILE *file = elf.empty() ? nullptr : std::fopen(elf.string().c_str(), "rb"))
            {
                std::vector<uint8_t> bytes;
                uint8_t chunk[65536];
                for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), file)) != 0;)
                    bytes.insert(bytes.end(), chunk, chunk + got);
                std::fclose(file);
                header.elfHash = hash64(bytes.data(), bytes.size());
            }
            header.flags = ps2x::det::enabled() ? 1u : 0u;
            header.maskCount = static_cast<uint32_t>(s.masks.size());
            copyName(header.run, sizeof(header.run), s.run);
            copyName(header.build, sizeof(header.build), s.build);
            copyName(header.elf, sizeof(header.elf), elf.filename().string());
            std::fwrite(&header, sizeof(header), 1, s.out);
            for (const auto &[address, bytes] : s.masks)
            {
                const uint32_t pair[2] = {address, bytes};
                std::fwrite(pair, sizeof(pair), 1, s.out);
            }
            std::fprintf(stderr, "[ref] recording %s (run %s, ELF hash %016llx%s)\n", s.path.c_str(), s.run.c_str(),
                         static_cast<unsigned long long>(header.elfHash), header.flags ? ", deterministic" : "");
            return true;
        }

        bool wantsDump(const State &s, uint64_t frame)
        {
            for (const auto &[first, last] : s.dumps)
                if (frame >= first && frame <= last)
                    return true;
            return false;
        }

        // PS2X_REFERENCE_CHECK_HOST=1: aligned 64-bit words of RAM that look like host pointers (into the host's copy
        // of RAM or the scratchpad, or near this module's own data), which a hash must never see.
        void checkHostValues(const uint8_t *ram, const uint8_t *scratchpad, uint64_t frame)
        {
            const uint64_t ramBase = reinterpret_cast<uintptr_t>(ram), spBase = reinterpret_cast<uintptr_t>(scratchpad);
            const uint64_t module = reinterpret_cast<uintptr_t>(&g_tracingCalls);
            uint32_t found = 0;
            for (uint32_t at = 0; at + 8u <= PS2_RAM_SIZE; at += 8u)
            {
                const uint64_t value = read64(ram + at);
                if (value < 0x10000ull)
                    continue;
                const bool host = (value >= ramBase && value < ramBase + PS2_RAM_SIZE) ||
                                  (value >= spBase && value < spBase + PS2_SCRATCHPAD_SIZE) ||
                                  (value + (256ull << 20) >= module && value < module + (256ull << 20));
                if (host && found++ < 8u)
                    std::fprintf(stderr, "[ref] frame %llu: RAM %08x holds %016llx, a host address?\n",
                                 static_cast<unsigned long long>(frame), at, static_cast<unsigned long long>(value));
            }
            std::fprintf(stderr, "[ref] frame %llu: %u RAM words look like host addresses\n", static_cast<unsigned long long>(frame),
                         found);
        }

        void writeCalls(State &s, uint64_t frame)
        {
            if (!s.callsOut)
            {
                s.callsOut = std::fopen((s.path + ".calls").c_str(), "w");
                if (!s.callsOut)
                    return;
            }
            std::fprintf(s.callsOut, "frame %llu calls %zu\n", static_cast<unsigned long long>(frame), s.calls.size());
            for (const Call &call : s.calls)
            {
                const auto it = s.nameIndex.find(call.pc);
                const std::string name = (call.id & 0x80000000u) && it == s.nameIndex.end()
                                             ? "syscall_" + std::to_string(static_cast<int32_t>(call.id << 1) >> 1)
                                             : (it != s.nameIndex.end() ? s.names[it->second] : std::string("?"));
                std::fprintf(s.callsOut, "  %-24s %08x %08x %08x %08x %08x -> %08x%s\n", name.c_str(), call.id, call.a[0],
                             call.a[1], call.a[2], call.a[3], call.v0,
                             (call.flags & kOpenAtFrameEnd) ? " (frame ended inside)" : (call.flags & kBlocked) ? " (blocked)" : "");
            }
        }
    }

    uint64_t hash64(const void *data, size_t bytes, uint64_t seed) noexcept
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        const uint8_t *const end = p + bytes;
        uint64_t h;
        if (bytes >= 32u)
        {
            uint64_t v1 = seed + kP1 + kP2, v2 = seed + kP2, v3 = seed, v4 = seed - kP1;
            const uint8_t *const limit = end - 32;
            do
            {
                v1 = round(v1, read64(p));
                v2 = round(v2, read64(p + 8));
                v3 = round(v3, read64(p + 16));
                v4 = round(v4, read64(p + 24));
                p += 32;
            } while (p <= limit);
            h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            h = mergeRound(h, v1);
            h = mergeRound(h, v2);
            h = mergeRound(h, v3);
            h = mergeRound(h, v4);
        }
        else
            h = seed + kP5;
        h += static_cast<uint64_t>(bytes);
        while (p + 8 <= end)
        {
            h ^= round(0, read64(p));
            h = rotl(h, 27) * kP1 + kP4;
            p += 8;
        }
        if (p + 4 <= end)
        {
            h ^= static_cast<uint64_t>(read32(p)) * kP1;
            h = rotl(h, 23) * kP2 + kP3;
            p += 4;
        }
        while (p < end)
        {
            h ^= static_cast<uint64_t>(*p) * kP5;
            h = rotl(h, 11) * kP1;
            ++p;
        }
        h ^= h >> 33;
        h *= kP2;
        h ^= h >> 29;
        h *= kP3;
        h ^= h >> 32;
        return h;
    }

    bool active() noexcept
    {
        State &s = state();
        if (!s.initialized)
            initialize(s);
        return s.active;
    }

    void setBuild(const char *build)
    {
        if (build)
            state().build = build;
    }

    void setCallNames(const CallName *names, size_t count)
    {
        ps2x::calllog::setCallNames(names, count); // PS2X_CALL_LOG: the names in its file
        State &s = state();
        if (!active())
            return;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t address = names[i].address & (PS2_RAM_SIZE - 1u);
            s.nameIndex[address] = static_cast<uint32_t>(s.names.size());
            s.names.emplace_back(names[i].name);
            s.ids.push_back(fnv1a32(names[i].name) & 0x7FFFFFFFu);
            s.traced[address >> 8] |= 1ull << ((address >> 2) & 63u);
        }
        std::fprintf(stderr, "[ref] %zu call addresses recorded\n", count);
    }

    bool tracesAddress(uint32_t pc) noexcept
    {
        const uint32_t address = pc & (PS2_RAM_SIZE - 1u);
        return (state().traced[address >> 8] >> ((address >> 2) & 63u)) & 1u;
    }

    void tracedCall(uint32_t pc, GuestFunction function, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &s = state();
        const uint32_t address = pc & (PS2_RAM_SIZE - 1u);
        const auto it = s.nameIndex.find(address);
        Call call;
        call.id = it != s.nameIndex.end() ? s.ids[it->second] : 0u;
        call.pc = address;
        for (int i = 0; i < 4; ++i)
            call.a[i] = getRegU32(ctx, 4 + i);
        const size_t slot = s.calls.size();
        const uint64_t generation = s.generation;
        s.calls.push_back(call);
        try
        {
            function(rdram, ctx, runtime);
        }
        catch (...)
        {
            if (s.generation == generation && slot < s.calls.size())
            {
                s.calls[slot].flags |= kBlocked;
                s.calls[slot].done = true;
            }
            throw;
        }
        if (s.generation == generation && slot < s.calls.size())
        {
            s.calls[slot].v0 = getRegU32(ctx, 2);
            s.calls[slot].done = true;
        }
    }

    size_t noteSyscallEntry(uint32_t number, const R5900Context *ctx)
    {
        State &s = state();
        Call call;
        call.id = 0x80000000u | (number & 0x7FFFFFFFu);
        for (int i = 0; i < 4; ++i)
            call.a[i] = getRegU32(ctx, 4 + i);
        s.calls.push_back(call);
        // The slot, tagged with the frame (generation) it belongs to.
        return (static_cast<size_t>(s.generation & 0xFFFFu) << 48) | (s.calls.size() - 1u);
    }

    void noteSyscallResult(size_t slot, const R5900Context *ctx, bool blocked)
    {
        State &s = state();
        const size_t index = slot & ((size_t{1} << 48) - 1u);
        if ((slot >> 48) != (s.generation & 0xFFFFu) || index >= s.calls.size())
            return;
        if (blocked)
            s.calls[index].flags |= kBlocked;
        else
            s.calls[index].v0 = getRegU32(ctx, 2);
        s.calls[index].done = true;
    }

    size_t tracedCallCount() noexcept
    {
        return state().calls.size();
    }

    void tracedCallsSince(size_t from, std::vector<TracedCall> &out)
    {
        State &s = state();
        for (size_t i = from; i < s.calls.size(); ++i)
        {
            const Call &c = s.calls[i];
            out.push_back(TracedCall{c.id, c.pc, {c.a[0], c.a[1], c.a[2], c.a[3]}, c.v0,
                                     c.flags | (c.done ? 0x80000000u : 0u)});
        }
    }

    void injectTracedCalls(const TracedCall *calls, size_t count)
    {
        State &s = state();
        for (size_t i = 0; i < count; ++i)
        {
            Call c;
            c.id = calls[i].id;
            c.pc = calls[i].pc;
            for (int k = 0; k < 4; ++k)
                c.a[k] = calls[i].a[k];
            c.v0 = calls[i].v0;
            c.flags = calls[i].flags & 0x7FFFFFFFu;
            c.done = (calls[i].flags & 0x80000000u) != 0u;
            s.calls.push_back(c);
        }
    }

    void noteDraws(uint64_t hash, uint32_t batches, uint32_t vertices)
    {
        State &s = state();
        if (!active())
            return;
        std::lock_guard<std::mutex> lock(s.drawMutex);
        const uint64_t pair[2] = {s.draws, hash};
        s.draws = hash64(pair, sizeof(pair));
        s.drawBatches += batches;
        s.drawVertices += vertices;
    }

    void frameEnd(PS2Runtime &runtime)
    {
        State &s = state();
        if (!active())
            return;
        if (!s.out && !openFile(s))
            return;
        const uint64_t frame = ps2x::det::frame();
        uint8_t *ram = runtime.memory().getRDRAM();
        uint8_t *scratchpad = runtime.memory().getScratchpad();
        Record record{};
        record.frame = static_cast<uint32_t>(frame);
        if (!s.masks.empty())
        {
            s.masked.assign(ram, ram + PS2_RAM_SIZE);
            for (const auto &[address, bytes] : s.masks)
                std::memset(s.masked.data() + address, 0, bytes);
            record.ram = hash64(s.masked.data(), PS2_RAM_SIZE);
        }
        else
            record.ram = hash64(ram, PS2_RAM_SIZE);
        record.scratchpad = hash64(scratchpad, PS2_SCRATCHPAD_SIZE);

        // Calls still open (the frame ends inside them) are this frame's, without a result.
        s.packed.clear();
        for (Call &call : s.calls)
        {
            if (!call.done)
                call.flags |= kOpenAtFrameEnd;
            s.packed.push_back(PackedCall{call.id, call.a[0], call.a[1], call.a[2], call.a[3], call.v0, call.flags});
        }
        record.calls = hash64(s.packed.data(), s.packed.size() * sizeof(PackedCall));
        record.callCount = static_cast<uint32_t>(s.calls.size());
        {
            std::lock_guard<std::mutex> lock(s.drawMutex);
            record.draws = s.draws;
            record.drawBatches = s.drawBatches;
            record.drawVertices = s.drawVertices;
            s.draws = 0;
            s.drawBatches = s.drawVertices = 0;
        }
        uint32_t samples = 0;
        ps2x::audio::NativeMixer::takeMixed(record.sound, samples);
        record.soundSamples = samples;
        int current = 0;
        runtime.eeScheduler().referenceThreads(current, s.ready);
        record.thread = current;
        s.threadWords.assign(1u, current);
        s.threadWords.insert(s.threadWords.end(), s.ready.begin(), s.ready.end());
        record.threads = hash64(s.threadWords.data(), s.threadWords.size() * sizeof(int32_t));
        record.eeCycle = runtime.eeScheduler().currentCycle();
        std::fwrite(&record, sizeof(record), 1, s.out);
        if (++s.records % 60u == 0u)
            std::fflush(s.out);

        if (frame >= s.callsFrom && frame < s.callsFrom + s.callsCount)
            writeCalls(s, frame);
        if (wantsDump(s, frame))
        {
            const std::string dump = s.path + ".ram." + std::to_string(frame);
            if (FILE *file = std::fopen(dump.c_str(), "wb"))
            {
                std::fwrite(ram, 1, PS2_RAM_SIZE, file);
                std::fwrite(scratchpad, 1, PS2_SCRATCHPAD_SIZE, file);
                std::fclose(file);
            }
        }
        if (s.checkHost && frame % 600u == 0u)
            checkHostValues(ram, scratchpad, frame);
        s.calls.clear();
        ++s.generation;

        if (s.framesLimit != 0u && s.records >= s.framesLimit)
        {
            std::fclose(s.out);
            s.out = nullptr;
            if (s.callsOut)
                std::fclose(s.callsOut);
            std::fprintf(stderr, "[ref] %llu frames recorded to %s: done\n", static_cast<unsigned long long>(s.records),
                         s.path.c_str());
            ps2x::capture::finish(); // PS2X_CAPTURE: its file completed first
            std::fflush(nullptr);
            std::_Exit(0);
        }
    }
}
