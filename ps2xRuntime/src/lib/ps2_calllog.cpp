// The call log and its self-check (runtime/ps2_calllog.h). The file format is BozoEngine's docs/specs/calllog.md
// ("PS2XCLG1").

#include "runtime/ps2_calllog.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_capture.h"
#include "runtime/ps2_determinism.h"
#include "runtime/ps2_reference.h"
#include "ps2_capture_common.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2x::capture
{
    // ps2_capture.cpp: the override addresses (port functions bound to guest addresses), in binding order.
    const std::vector<uint32_t> &overrideAddresses() noexcept;
    // The write-tracked main RAM (nullptr when it is not).
    uint8_t *watchedRam() noexcept;
}

namespace ps2x::calllog
{
    bool g_on = false;

    namespace
    {
        using namespace ps2x::capture::common;
        namespace watch = ps2x::capture::watch;

        constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'C', 'L', 'G', '1'};
        constexpr uint32_t kVersion = 1;
        constexpr uint32_t kHeaderBytes = 128;
        constexpr uint32_t kRecordHeaderBytes = 40;
        constexpr uint32_t kTagNames = tag("NAME");
        constexpr uint32_t kTagOverrides = tag("OVRD");
        constexpr uint32_t kTagRecords = tag("RECS");
        constexpr uint32_t kTagEnd = tag("END ");
        constexpr size_t kChunkRawBytes = 4u << 20;

        enum Type : uint8_t
        {
            kCall = 1,
            kMem = 2,
            kCtx = 3,
            kInvoke = 4,
            kInvokeEnd = 5,
            kSwitch = 6,
            kFrame = 7,
            kOpRecord = 8,
            kInit = 9,
        };

        // Where a change outside guest code comes from (MEM records' source, OP records' origin).
        enum Source : uint32_t
        {
            kSrcNone = 0,
            kSrcCall = 1,         // inside an out-call (OP records only: the call's own effects are in its record)
            kSrcIop = 2,          // an IOP slice run as EE cycles were accounted
            kSrcIopVblank = 3,    // the IOP's catch-up at a vblank
            kSrcEvents = 4,       // the scheduler's events: vblank, timers, alarms, posted tasks
            kSrcResume = 5,       // a wait's completion as its thread resumes
            kSrcHandlerEnd = 6,   // a handler's completion as it returns
            kSrcHandlerStart = 7, // a handler set up on a thread
            kSrcThreadEnd = 8,    // a thread's end (its observers)
            kSrcDispatcher = 9,   // the rest of the dispatcher's work (strict mode)
            kSrcTask = 10,        // a posted task running (OP records only; its memory is in the events')
        };

        // CALL record flags.
        enum CallFlags : uint8_t
        {
            kCallUnwound = 1u << 0,     // left through the dispatcher (the thread blocked or switched, a handler began)
            kCallFrameInside = 1u << 1, // the game frame ended inside the call (a FRAME record follows it)
            kCallCut = 1u << 2,         // the run ended inside the call
            kCallContextLost = 1u << 3, // unwound and its context no longer exists: no context words
        };
        // Any record but CALL: made while the CALL record before it was open (its seq is that call's).
        constexpr uint8_t kInsideCall = 0x80u;

        enum class Mode
        {
            Off,
            Record,
            Check,
        };

        enum class Where
        {
            Guest,
            Call,
            Dispatcher,
        };

        // The EE kernel's calls (BozoEngine implements them natively): NAME records' class 1.
        const std::unordered_set<std::string> &kernelNames()
        {
            static const std::unordered_set<std::string> names = {
                "ResetEE", "SetGsCrt", "AddIntcHandler", "AddIntcHandler2", "RemoveIntcHandler", "AddDmacHandler",
                "AddDmacHandler2", "RemoveDmacHandler", "ps2__EnableIntc", "ps2__DisableIntc", "ps2__EnableDmac",
                "ps2__DisableDmac", "ps2__SetAlarm", "ps2__ReleaseAlarm", "_iEnableIntc", "_iDisableIntc",
                "_iEnableDmac", "_iDisableDmac", "_iSetAlarm", "_iReleaseAlarm", "CreateThread", "DeleteThread",
                "StartThread", "ExitThread", "ExitDeleteThread", "TerminateThread", "ChangeThreadPriority",
                "iChangeThreadPriority", "RotateThreadReadyQueue", "_iRotateThreadReadyQueue", "ReleaseWaitThread",
                "iReleaseWaitThread", "GetThreadId", "ReferThreadStatus", "iReferThreadStatus", "SleepThread",
                "WakeupThread", "_iWakeupThread", "CancelWakeupThread", "iCancelWakeupThread", "SuspendThread",
                "ResumeThread", "EndOfHeap", "CreateSema", "DeleteSema", "SignalSema", "iSignalSema", "WaitSema",
                "PollSema", "iPollSema", "ReferSemaStatus", "iReferSemaStatus", "SetOsdConfigParam",
                "GetOsdConfigParam", "EnableCache", "DisableCache", "FlushCache", "iFlushCache", "SetOsdConfigParam2",
                "GetOsdConfigParam2", "GsGetIMR", "iGsGetIMR", "GsPutIMR", "iGsPutIMR", "SetVSyncFlag",
                "GetMemorySize", "DisableIntc", "EnableIntc", "DisableDmac", "EnableDmac", "SetAlarm", "ReleaseAlarm",
                "iEnableIntc", "iDisableIntc", "iEnableDmac", "iDisableDmac", "iSetAlarm", "iReleaseAlarm",
                "InitThread", "iWakeupThread", "iRotateThreadReadyQueue"};
            return names;
        }

        struct OpenCall
        {
            bool open = false;
            uint32_t id = 0;
            Where prev = Where::Guest;
            int uncaught = 0;
            uint8_t flags = 0;
            R5900Context *ctx = nullptr;
            uint32_t words[kCtxWords];
            Buf rec;         // the record, its out-call part completed at the end
            size_t outAt = 0; // where the out-call part starts
            int thread = 0;
        };

        // The expected log, in the self-check.
        struct Expected
        {
            FILE *in = nullptr;
            std::vector<uint8_t> raw;
            std::vector<uint8_t> packed;
            size_t at = 0;
            bool ended = false;

            // The next record (nullptr at the end), not consumed.
            const uint8_t *peek()
            {
                while (at >= raw.size())
                {
                    if (ended || !in)
                        return nullptr;
                    uint32_t chunk[4];
                    if (std::fread(chunk, sizeof(chunk), 1, in) != 1)
                    {
                        ended = true;
                        return nullptr;
                    }
                    packed.resize(chunk[2]);
                    if (chunk[2] && std::fread(packed.data(), 1, chunk[2], in) != chunk[2])
                    {
                        ended = true;
                        return nullptr;
                    }
                    if (chunk[0] != kTagRecords)
                        continue;
                    if (chunk[1] & 1u)
                    {
                        raw.resize(chunk[3]);
                        uLongf rawBytes = chunk[3];
                        if (uncompress(raw.data(), &rawBytes, packed.data(), chunk[2]) != Z_OK || rawBytes != chunk[3])
                        {
                            ended = true;
                            raw.clear();
                            return nullptr;
                        }
                    }
                    else
                        raw = packed;
                    at = 0;
                }
                return raw.data() + at;
            }
            void next()
            {
                if (const uint8_t *p = peek())
                    at += rd32(p + 4);
            }
        };

        struct State
        {
            Mode mode = Mode::Off;
            bool configured = false;
            bool strict = false;
            std::string path;
            PS2Runtime *runtime = nullptr;
            EeScheduler *scheduler = nullptr;
            uint8_t *ram = nullptr;
            uint8_t *spr = nullptr;
            uint8_t *vu0 = nullptr;
            bool watchBroken = false;
            bool bound = false;
            std::vector<uint8_t> shadow;
            std::vector<uint32_t> pages;

            // Windows.
            Where where = Where::Dispatcher;
            bool synced = false;
            uint32_t source = kSrcNone; // the dispatcher phase open (none: no phase)
            bool iopOpen = false;
            uint32_t taskDepth = 0;

            uint64_t calls = 0;
            uint32_t checkpoints = 0;
            uint64_t records = 0;
            OpenCall call;
            uint32_t nextId = 1;
            Buf pending; // records made while a call is open: written after it
            Buf chunk;
            FILE *out = nullptr;
            uint64_t bytes = 0;
            bool namesWritten = false;
            std::vector<std::pair<uint32_t, std::string>> names;
            bool overridesDirty = true;

            // The context each (thread, depth) had when guest code last stopped running on it.
            std::unordered_map<uint64_t, std::array<uint32_t, kCtxWords>> seen;
            int lastThread = 0;
            uint32_t lastDepth = 0;
            bool ranAny = false;

            // Origins of waits and invocations (the call that made them), for MEM records of their completions.
            std::unordered_map<int, uint64_t> waitOrigin;
            std::unordered_map<uint64_t, uint64_t> invocationOrigin;
            uint64_t completionOrigin = ~0ull;

            // Statistics.
            uint64_t memRecords[16] = {};
            uint64_t memBytes[16] = {};
            uint64_t callBytes = 0;
            uint64_t typeCounts[16] = {};
            std::map<uint32_t, uint64_t> callsByTarget;

            // The self-check.
            Expected expected;
            bool desynced = false;
            uint64_t compared = 0, matched = 0, differ = 0, substituted = 0;
            uint64_t differByType[16] = {};
            int logged = 0;
            std::string report;
        };

        State &state()
        {
            static State s;
            return s;
        }

        uint64_t contextKey(int thread, uint32_t depth)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(thread)) << 32) | depth;
        }

        uint8_t *flatPage(State &s, uint32_t page)
        {
            if (page < kRamPages)
                return s.ram + size_t(page) * kPage;
            if (page < kRamPages + kSprBytes / kPage)
                return s.spr + size_t(page - kRamPages) * kPage;
            return s.vu0 + size_t(page - kRamPages - kSprBytes / kPage) * kPage;
        }

        uint8_t *flatByte(State &s, uint32_t address)
        {
            if (address < kFlatSpr)
                return s.ram + address;
            if (address < kFlatVu0)
                return s.spr + (address - kFlatSpr);
            return s.vu0 + (address - kFlatVu0);
        }

        // The pages written since the last collect (RAM from the write tracking; the scratchpad and VU0's data memory
        // always).
        void collect(State &s)
        {
            s.pages.resize(kPages);
            size_t count = SIZE_MAX;
            if (!s.watchBroken)
                count = watch::collect(s.ram, kRamBytes, s.pages.data(), kRamPages);
            if (count == SIZE_MAX)
            {
                count = kRamPages;
                for (uint32_t p = 0; p < kRamPages; ++p)
                    s.pages[p] = p;
            }
            for (uint32_t p = kRamPages; p < kPages; ++p)
                s.pages[count++] = p;
            s.pages.resize(count);
        }

        void syncShadow(State &s)
        {
            collect(s);
            for (uint32_t p : s.pages)
                std::memcpy(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), kPage);
            s.synced = true;
        }

        // What memory changed since the shadow was synced, as runs (the shadow catches up).
        uint32_t diffRuns(State &s, Buf &out)
        {
            collect(s);
            uint32_t runs = 0;
            for (uint32_t p : s.pages)
                runs += diffPage(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), p * kPage, out);
            return runs;
        }

        void shadowCatchUp(State &s)
        {
            for (uint32_t p : s.pages)
                std::memcpy(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), kPage);
        }

        // The self-check: what the run wrote to the collected pages is thrown away (back to the shadow) and the log's
        // runs are applied instead.
        void substituteRuns(State &s, const uint8_t *runs, uint32_t count)
        {
            for (uint32_t p : s.pages)
                std::memcpy(flatPage(s, p), s.shadow.data() + size_t(p) * kPage, kPage);
            const uint8_t *q = runs;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t address = rd32(q), len = rd32(q + 4);
                if (address + len <= kFlatBytes)
                    for (uint32_t j = 0; j < len; ++j)
                        *flatByte(s, address + j) = q[8 + j];
                q += 8 + ((len + 3u) & ~3u);
            }
            ++s.substituted;
        }

        // ---- Output ---------------------------------------------------------------------------------------------------
        void writeChunk(State &s, uint32_t chunkTag, const Buf &raw, bool compress)
        {
            if (!s.out)
                return;
            std::vector<uint8_t> packed;
            uint32_t flags = 0;
            const uint8_t *data = raw.b.data();
            uint32_t stored = static_cast<uint32_t>(raw.size());
            if (compress && raw.size() > 64u)
            {
                uLongf packedBytes = compressBound(static_cast<uLong>(raw.size()));
                packed.resize(packedBytes);
                if (compress2(packed.data(), &packedBytes, raw.b.data(), static_cast<uLong>(raw.size()), 3) == Z_OK &&
                    packedBytes < raw.size())
                {
                    flags = 1;
                    data = packed.data();
                    stored = static_cast<uint32_t>(packedBytes);
                }
            }
            const uint32_t header[4] = {chunkTag, flags, stored, static_cast<uint32_t>(raw.size())};
            std::fwrite(header, sizeof(header), 1, s.out);
            std::fwrite(data, 1, stored, s.out);
            s.bytes += sizeof(header) + stored;
        }

        void flushChunk(State &s)
        {
            if (s.chunk.size() == 0)
                return;
            writeChunk(s, kTagRecords, s.chunk, true);
            s.chunk.b.clear();
        }

        void writeNames(State &s)
        {
            if (!s.out || s.names.empty())
                return;
            flushChunk(s);
            Buf b;
            b.u32(static_cast<uint32_t>(s.names.size()));
            const auto &kernel = kernelNames();
            const auto &overrides = ps2x::capture::overrideAddresses();
            for (const auto &[address, name] : s.names)
            {
                b.u32(address);
                uint32_t kind = kernel.count(name) ? 1u : 2u;
                if (std::find(overrides.begin(), overrides.end(), address) != overrides.end())
                    kind = 3u;
                b.u32(kind);
                b.u32(static_cast<uint32_t>(name.size()));
                b.put(name.data(), name.size());
                b.pad4();
            }
            writeChunk(s, kTagNames, b, true);
            s.namesWritten = true;
        }

        void writeOverrides(State &s)
        {
            if (!s.out)
                return;
            flushChunk(s);
            const auto &list = ps2x::capture::overrideAddresses();
            Buf b;
            b.u32(static_cast<uint32_t>(list.size()));
            for (uint32_t a : list)
                b.u32(a);
            writeChunk(s, kTagOverrides, b, false);
            s.overridesDirty = false;
        }

        bool openFile(State &s)
        {
            s.out = std::fopen(s.path.c_str(), "wb");
            if (!s.out)
            {
                std::fprintf(stderr, "[calllog] cannot write %s: call log off\n", s.path.c_str());
                return false;
            }
            uint8_t header[kHeaderBytes] = {};
            std::memcpy(header, kMagic, 8);
            uint32_t frameLimit = 0;
            if (const char *v = std::getenv("PS2X_REFERENCE_FRAMES"); v && *v)
                frameLimit = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
            const uint32_t fields[] = {kVersion, kHeaderBytes, kCtxBytes, kFlatBytes, kRamBytes, kSprBytes, kVu0Bytes,
                                       kPage, frameLimit, s.strict ? 1u : 0u};
            std::memcpy(header + 8, fields, sizeof(fields));
            uint64_t elfHash = 0;
            const std::filesystem::path elf = PS2Runtime::getIoPaths().elfPath;
            if (!elf.empty())
            {
                std::ifstream in(elf, std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!bytes.empty())
                    elfHash = ps2x::reference::hash64(bytes.data(), bytes.size());
            }
            std::memcpy(header + 48, &elfHash, 8);
            const uint32_t clock = static_cast<uint32_t>(EeScheduler::kEeClockHz);
            std::memcpy(header + 56, &clock, 4);
            std::memcpy(header + 60, &kRecordHeaderBytes, 4);
            std::string run = std::filesystem::path(s.path).stem().string();
            if (const char *name = std::getenv("PS2X_REFERENCE_RUN"); name && *name)
                run = name;
            std::strncpy(reinterpret_cast<char *>(header + 64), run.c_str(), 63);
            std::fwrite(header, 1, sizeof(header), s.out);
            s.bytes = sizeof(header);
            std::fprintf(stderr, "[calllog] recording %s%s\n", s.path.c_str(), s.strict ? " (strict)" : "");
            return true;
        }

        // ---- Records --------------------------------------------------------------------------------------------------
        void head(State &s, Buf &b, uint8_t type, uint8_t flags, uint16_t sub)
        {
            b.u8(type);
            b.u8(flags);
            b.u16(sub);
            b.u32(0); // size, patched by finishRecord
            b.u64(s.scheduler ? s.scheduler->currentCycle() : 0u);
            b.u64(s.calls);
            b.u32(static_cast<uint32_t>(ps2x::det::frame()));
            const int thread = s.scheduler ? s.scheduler->currentThreadId() : 0;
            b.u32(static_cast<uint32_t>(thread));
            b.u32(s.checkpoints);
            uint32_t pc = 0;
            if (s.scheduler && thread != 0)
                if (const GuestThread *t = s.scheduler->thread(thread))
                    pc = t->activeContext().pc;
            b.u32(pc);
        }

        void finishRecord(Buf &b, size_t at)
        {
            b.pad4();
            b.patch32(at + 4, static_cast<uint32_t>(b.size() - at));
        }

        std::string describe(const uint8_t *r)
        {
            static const char *const types[] = {"?", "CALL", "MEM", "CTX", "INVOKE", "INVOKE_END", "SWITCH", "FRAME",
                                                "OP", "INIT"};
            char text[200];
            std::snprintf(text, sizeof(text), "%s sub %u seq %" PRIu64 " frame %u cycle %" PRIu64 " thread %d",
                          r[0] < 10 ? types[r[0]] : "?", static_cast<unsigned>(r[2] | (r[3] << 8)), rd64(r + 16),
                          rd32(r + 24), rd64(r + 8), static_cast<int>(rd32(r + 28)));
            std::string out = text;
            if (r[0] == kCall)
            {
                std::snprintf(text, sizeof(text), " kind %u target 0x%x", r[96], rd32(r + 100));
                out += text;
            }
            return out;
        }

        void note(State &s, const std::string &line)
        {
            s.report += line + "\n";
            if (s.logged++ < 30)
                std::fprintf(stderr, "[calllog-check] %s\n", line.c_str());
        }

        // The self-check: the record the run made, compared with the log's next one (consumed). Returns the log's
        // record when they are the same record (same type, point and target), whatever their contents, else nullptr.
        const uint8_t *compareRecord(State &s, const uint8_t *made, size_t bytes)
        {
            if (s.desynced)
                return nullptr;
            const uint8_t *want = s.expected.peek();
            ++s.compared;
            ++s.typeCounts[made[0] & 15u];
            if (!want)
            {
                note(s, "the log ended before " + describe(made));
                s.desynced = true;
                return nullptr;
            }
            const uint32_t wantBytes = rd32(want + 4);
            const bool sameRecord = want[0] == made[0] && std::memcmp(want + 2, made + 2, 2) == 0 &&
                                    std::memcmp(want + 8, made + 8, 32) == 0 &&
                                    (made[0] != kCall || std::memcmp(want + 96, made + 96, 24) == 0);
            if (!sameRecord)
            {
                note(s, "desynchronized: the run made " + describe(made) + ", the log has " + describe(want));
                s.desynced = true;
                return nullptr;
            }
            s.expected.next();
            if (wantBytes == bytes && std::memcmp(want, made, bytes) == 0)
                ++s.matched;
            else
            {
                ++s.differ;
                ++s.differByType[made[0] & 15u];
                note(s, "contents differ: " + describe(made));
            }
            return want;
        }

        // A finished record: to the file (or, in the self-check, compared). Records made while a call is open wait
        // for its record.
        void emit(State &s, Buf &b)
        {
            ++s.records;
            if (s.call.open)
            {
                b.b[1] |= kInsideCall;
                s.pending.put(b.b.data(), b.size());
                return;
            }
            if (s.mode == Mode::Check)
            {
                (void)compareRecord(s, b.b.data(), b.size());
                return;
            }
            ++s.typeCounts[b.b[0] & 15u];
            s.chunk.put(b.b.data(), b.size());
            if (s.chunk.size() >= kChunkRawBytes)
                flushChunk(s);
        }

        void emitPending(State &s)
        {
            if (s.pending.size() == 0)
                return;
            const uint8_t *p = s.pending.b.data();
            const uint8_t *end = p + s.pending.size();
            while (p < end)
            {
                const uint32_t bytes = rd32(p + 4);
                if (s.mode == Mode::Check)
                    (void)compareRecord(s, p, bytes);
                else
                {
                    ++s.typeCounts[p[0] & 15u];
                    s.chunk.put(p, bytes);
                }
                p += bytes;
            }
            s.pending.b.clear();
            if (s.chunk.size() >= kChunkRawBytes)
                flushChunk(s);
        }

        // ---- Windows --------------------------------------------------------------------------------------------------
        // The changes made since the shadow was synced, as a MEM record of the open source.
        void flushWindow(State &s, uint32_t source)
        {
            if (!s.synced)
                return;
            Buf runs;
            const uint32_t count = diffRuns(s, runs);
            if (count == 0)
            {
                shadowCatchUp(s);
                return;
            }
            Buf b;
            head(s, b, kMem, 0, static_cast<uint16_t>(source));
            b.u32(count);
            b.u32(0);
            const uint64_t origin = (source == kSrcResume || source == kSrcHandlerEnd) ? s.completionOrigin : ~0ull;
            b.u64(origin);
            b.put(runs.b.data(), runs.size());
            finishRecord(b, 0);
            s.memRecords[source & 15u] += 1;
            s.memBytes[source & 15u] += runs.size();
            if (s.mode == Mode::Check && !s.call.open)
            {
                if (const uint8_t *want = compareRecord(s, b.b.data(), b.size()))
                    substituteRuns(s, want + 56, rd32(want + 40));
                shadowCatchUp(s);
                ++s.records;
                return;
            }
            shadowCatchUp(s);
            emit(s, b);
        }

        void ensureSynced(State &s)
        {
            if (!s.synced)
                syncShadow(s);
        }

        uint32_t currentOrigin(const State &s)
        {
            if (s.call.open)
                return kSrcCall;
            if (s.iopOpen)
                return kSrcIop;
            if (s.taskDepth)
                return kSrcTask;
            if (s.source != kSrcNone)
                return s.source;
            // Outside a phase: the dispatcher's own work, or (0) an EE checkpoint inside guest code.
            return s.where == Where::Dispatcher ? kSrcDispatcher : kSrcNone;
        }

        void bindNow(State &s, PS2Runtime *runtime)
        {
            if (s.bound || !runtime)
                return;
            s.bound = true;
            s.runtime = runtime;
            s.scheduler = &runtime->eeScheduler();
            s.ram = runtime->memory().getRDRAM();
            s.spr = runtime->memory().getScratchpad();
            s.vu0 = runtime->memory().getVU0Data();
            if (s.ram != ps2x::capture::watchedRam())
            {
                std::fprintf(stderr, "[calllog] main RAM is not write-tracked: every page is compared each time\n");
                s.watchBroken = true;
            }
            s.shadow.assign(kFlatBytes, 0);
            if (s.mode == Mode::Record && !openFile(s))
            {
                s.mode = Mode::Off;
                g_on = false;
                return;
            }
            writeOverrides(s);
            writeNames(s);
            // INIT: the memory as the run starts (all zeros elsewhere).
            Buf runs;
            uint32_t count = 0;
            collect(s);
            for (uint32_t p = 0; p < kPages; ++p)
                count += diffPage(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), p * kPage, runs);
            for (uint32_t p = 0; p < kPages; ++p)
                std::memcpy(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), kPage);
            Buf b;
            head(s, b, kInit, 0, 0);
            b.u32(count);
            b.u32(0);
            b.put(runs.b.data(), runs.size());
            finishRecord(b, 0);
            emit(s, b);
            s.synced = false;
            s.where = Where::Dispatcher;
        }

        void readEnvironment(State &s)
        {
            if (const char *v = std::getenv("PS2X_CALL_LOG_STRICT"); v && *v == '1')
                s.strict = true;
        }

        void writeSummary(State &s)
        {
            static const char *const sources[] = {"-", "call", "iop", "iop-vblank", "events", "resume", "handler-end",
                                                  "handler-start", "thread-end", "dispatcher", "task"};
            const std::string path = s.path + ".summary.txt";
            FILE *f = std::fopen(path.c_str(), "w");
            if (!f)
                return;
            static const char *const types[] = {"?", "CALL", "MEM", "CTX", "INVOKE", "INVOKE_END", "SWITCH", "FRAME",
                                                "OP", "INIT"};
            std::fprintf(f, "# records by type\n");
            for (int i = 1; i < 10; ++i)
                std::fprintf(f, "%s %" PRIu64 "\n", types[i], s.typeCounts[i]);
            std::fprintf(f, "# MEM records by source: records, bytes of runs\n");
            for (int i = 1; i < 11; ++i)
                if (s.memRecords[i])
                    std::fprintf(f, "%s %" PRIu64 " %" PRIu64 "\n", sources[i], s.memRecords[i], s.memBytes[i]);
            std::fprintf(f, "# calls by target: target count name\n");
            std::unordered_map<uint32_t, std::string> names;
            for (const auto &[a, n] : s.names)
                names[a] = n;
            for (const auto &[target, count] : s.callsByTarget)
            {
                const uint32_t kind = target >> 28;
                const uint32_t address = target & 0x0FFFFFFFu;
                const auto it = names.find(address);
                std::fprintf(f, "%u 0x%07x %" PRIu64 " %s\n", kind, address, count,
                             it != names.end() ? it->second.c_str() : "");
            }
            std::fclose(f);
        }
    }

    // ---- Public -------------------------------------------------------------------------------------------------------
    bool configure() noexcept
    {
        State &s = state();
        if (s.configured)
            return s.mode != Mode::Off;
        s.configured = true;
        const char *record = std::getenv("PS2X_CALL_LOG");
        const char *check = std::getenv("PS2X_CALL_LOG_CHECK");
        if ((record && *record) || (check && *check))
        {
            if (!ps2x::det::enabled())
            {
                std::fprintf(stderr, "[calllog] needs PS2X_DETERMINISTIC=1: off\n");
                return false;
            }
            readEnvironment(s);
            if (check && *check)
            {
                s.mode = Mode::Check;
                s.path = check;
                s.expected.in = std::fopen(check, "rb");
                uint8_t header[kHeaderBytes];
                if (!s.expected.in || std::fread(header, 1, kHeaderBytes, s.expected.in) != kHeaderBytes ||
                    std::memcmp(header, kMagic, 8) != 0)
                {
                    std::fprintf(stderr, "[calllog-check] %s: not a PS2XCLG1 log\n", check);
                    if (s.expected.in)
                        std::fclose(s.expected.in);
                    s.expected.in = nullptr;
                    s.mode = Mode::Off;
                    return false;
                }
                std::fprintf(stderr, "[calllog-check] checking against %s%s\n", check, s.strict ? " (strict)" : "");
            }
            else
            {
                s.mode = Mode::Record;
                s.path = record;
            }
            std::atexit([] { finish(); });
            g_on = true;
        }
        return s.mode != Mode::Off;
    }

    void setCallNames(const ps2x::reference::CallName *names, size_t count)
    {
        State &s = state();
        if (!configure())
            return;
        for (size_t i = 0; i < count; ++i)
            s.names.emplace_back(names[i].address & (PS2_RAM_SIZE - 1u), names[i].name);
        std::sort(s.names.begin(), s.names.end());
        if (s.out)
            writeNames(s);
    }

    void overridesChanged() noexcept
    {
        State &s = state();
        if (s.out)
            writeOverrides(s);
    }

    bool strict() noexcept { return state().strict; }

    void bind(PS2Runtime *runtime) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off)
            return;
        bindNow(s, runtime);
    }

    void checkpoint() noexcept
    {
        ++state().checkpoints;
    }

    bool callEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, capture::OutKind kind, uint32_t target,
                   uint32_t &token)
    {
        (void)rdram;
        State &s = state();
        if (s.mode == Mode::Off)
            return false;
        bindNow(s, runtime);
        OpenCall &c = s.call;
        if (c.open || !s.bound)
            return false;
        if (s.where == Where::Dispatcher && s.source != kSrcNone)
        {
            flushWindow(s, s.source);
            s.source = kSrcNone;
        }
        ensureSynced(s);
        c.open = true;
        c.id = s.nextId++;
        if (c.id == 0)
            c.id = s.nextId++;
        c.prev = s.where;
        c.uncaught = std::uncaught_exceptions();
        c.flags = 0;
        c.ctx = ctx;
        c.thread = s.scheduler->currentThreadId();
        toRecord(*ctx, c.words);
        c.rec.b.clear();
        head(s, c.rec, kCall, 0, 0);
        c.rec.u32(getRegU32(ctx, 31));
        c.rec.u32(getRegU32(ctx, 29));
        for (int i = 0; i < 4; ++i)
            c.rec.u32(getRegU32(ctx, 8 + i));
        c.rec.u32(c.words[138 + 12]);
        c.rec.u32(c.words[138 + 13]);
        c.rec.u64(0); // cycle after
        c.rec.u32(static_cast<uint32_t>(runtime->memory().pendingTimerSpinCycles()));
        c.rec.u32(0); // spin after
        uint32_t depth = 0;
        if (const GuestThread *t = s.scheduler->thread(c.thread))
            depth = static_cast<uint32_t>(t->invocations.size());
        c.rec.u32(depth);
        c.rec.u32(0);
        c.outAt = c.rec.size();
        c.rec.u8(static_cast<uint8_t>(kind));
        c.rec.u8(0);
        c.rec.u16(0);
        c.rec.u32(target);
        uint32_t value[4] = {};
        if (kind == capture::kOutStub || kind == capture::kOutOverride)
            for (int i = 0; i < 4; ++i)
                value[i] = getRegU32(ctx, 4 + i);
        else if (kind == capture::kOutSyscall)
        {
            value[0] = getRegU32(ctx, 3);
            for (int i = 0; i < 3; ++i)
                value[1 + i] = getRegU32(ctx, 4 + i);
        }
        c.rec.put(value, 16);
        c.rec.u32(0);
        c.rec.u32(0);
        c.rec.u32(0);
        s.where = Where::Call;
        ++s.callsByTarget[(uint32_t(kind) << 28) | (target & 0x0FFFFFFFu)];
        token = c.id;
        return false;
    }

    bool ioEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, capture::OutKind kind, uint32_t address,
                 uint32_t size, void *value, uint32_t &token)
    {
        if (callEnter(rdram, ctx, runtime, kind, address, token))
            return true;
        State &s = state();
        if (token)
        {
            OpenCall &c = s.call;
            c.rec.b[c.outAt + 1] = static_cast<uint8_t>(size);
            if (kind == capture::kOutIoStore)
                std::memcpy(c.rec.b.data() + c.outAt + 8, value, std::min<uint32_t>(size, 16u));
        }
        return false;
    }

    void ioValue(uint32_t token, const void *value, uint32_t size) noexcept
    {
        State &s = state();
        OpenCall &c = s.call;
        if (!token || !c.open || c.id != token)
            return;
        std::memcpy(c.rec.b.data() + c.outAt + 8, value, std::min<uint32_t>(size, 16u));
    }

    void callLeave(uint32_t token) noexcept
    {
        State &s = state();
        OpenCall &c = s.call;
        if (!c.open || c.id != token)
            return;
        try
        {
            const bool unwound = std::uncaught_exceptions() > c.uncaught;
            if (unwound)
                c.flags |= kCallUnwound;
            bool contextAlive = true;
            if (unwound)
                contextAlive = s.scheduler->ownsContext(c.ctx);
            uint32_t mask[kMaskWords] = {};
            std::vector<uint32_t> words;
            uint32_t now[kCtxWords];
            if (contextAlive)
            {
                toRecord(*c.ctx, now);
                diffContext(c.words, now, mask, words);
            }
            else
                c.flags |= kCallContextLost;
            Buf &b = c.rec;
            b.put(mask, sizeof(mask));
            b.put(words.data(), words.size() * 4);
            const size_t runsAt = b.size();
            const uint32_t runs = diffRuns(s, b);
            b.patch32(c.outAt + 24, static_cast<uint32_t>(words.size()));
            b.patch32(c.outAt + 28, runs);
            const uint64_t after = s.scheduler->currentCycle();
            std::memcpy(b.b.data() + 72, &after, 8);
            const uint32_t spinAfter = static_cast<uint32_t>(s.runtime->memory().pendingTimerSpinCycles());
            std::memcpy(b.b.data() + 84, &spinAfter, 4);
            b.b[1] = c.flags;
            finishRecord(b, 0);
            s.callBytes += b.size();
            c.open = false;
            if (s.mode == Mode::Check)
            {
                if (const uint8_t *want = compareRecord(s, b.b.data(), b.size()))
                {
                    // The call's effects as the log has them: its memory runs, and the context at entry with its words.
                    const uint8_t *out = want + 96;
                    const uint32_t ctxCount = rd32(out + 24);
                    const uint32_t runCount = rd32(out + 28);
                    const uint32_t *wantMask = reinterpret_cast<const uint32_t *>(out + kOutHeaderBytes);
                    const uint32_t *wantWords = wantMask + kMaskWords;
                    const uint8_t *wantRuns = reinterpret_cast<const uint8_t *>(wantWords + ctxCount);
                    substituteRuns(s, wantRuns, runCount);
                    if (contextAlive && !(want[1] & kCallContextLost))
                    {
                        uint32_t rec[kCtxWords];
                        std::memcpy(rec, c.words, kCtxBytes);
                        applyContextWords(wantMask, wantWords, rec);
                        fromRecord(rec, *c.ctx);
                    }
                }
                (void)runsAt;
                shadowCatchUp(s);
                ++s.records;
            }
            else
            {
                shadowCatchUp(s);
                ++s.records;
                ++s.typeCounts[kCall];
                s.chunk.put(b.b.data(), b.size());
            }
            ++s.calls;
            s.checkpoints = 0;
            emitPending(s);
        }
        catch (...)
        {
            c.open = false;
        }
        s.where = c.prev;
        if (s.where == Where::Guest)
            s.synced = false;
    }

    void phaseBegin(uint32_t source) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound || s.call.open)
            return;
        if (s.source != kSrcNone && s.source != source)
            flushWindow(s, s.source);
        ensureSynced(s);
        s.source = source;
        if (source == kSrcResume && s.scheduler)
        {
            const auto it = s.waitOrigin.find(s.scheduler->currentThreadId());
            s.completionOrigin = it != s.waitOrigin.end() ? it->second : ~0ull;
            if (it != s.waitOrigin.end())
                s.waitOrigin.erase(it);
        }
    }

    void phaseEnd() noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound || s.call.open || s.source == kSrcNone)
            return;
        flushWindow(s, s.source);
        s.source = s.strict ? kSrcDispatcher : kSrcNone;
        // Lazy: what the dispatcher writes outside a phase is not followed (strict mode would show it).
        s.synced = s.source != kSrcNone;
    }

    void beforeGuest(EeScheduler &scheduler, int thread, uint32_t depth, R5900Context &context) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        try
        {
            if (s.source != kSrcNone)
                flushWindow(s, s.source);
            s.source = kSrcNone;
            // A thread switch.
            if (!s.ranAny || thread != s.lastThread || depth != s.lastDepth)
            {
                uint32_t reason = 0, waitReason = 0, waitObject = 0;
                uint64_t token = 0;
                if (!s.ranAny || s.lastThread == 0)
                    reason = 6;
                else if (thread == s.lastThread)
                    reason = depth > s.lastDepth ? 7 : 8;
                else if (const GuestThread *p = scheduler.thread(s.lastThread))
                {
                    switch (p->status)
                    {
                    case EeThreadStatus::Waiting:
                    case EeThreadStatus::WaitingSuspended:
                        reason = 1;
                        waitReason = static_cast<uint32_t>(p->wait.reason);
                        if (const auto *sema = std::get_if<EeSemaphoreWait>(&p->wait.payload))
                            waitObject = static_cast<uint32_t>(sema->id);
                        else if (const auto *flag = std::get_if<EeEventFlagWait>(&p->wait.payload))
                            waitObject = static_cast<uint32_t>(flag->id);
                        else if (const auto *vsync = std::get_if<EeVSyncWait>(&p->wait.payload))
                            token = vsync->afterTick;
                        else if (const auto *external = std::get_if<EeExternalWait>(&p->wait.payload))
                        {
                            waitObject = external->type;
                            token = external->token;
                        }
                        break;
                    case EeThreadStatus::Ready:
                        reason = 2;
                        break;
                    case EeThreadStatus::Dormant:
                        reason = 3;
                        break;
                    case EeThreadStatus::Suspended:
                        reason = 4;
                        break;
                    default:
                        reason = 9;
                        break;
                    }
                }
                else
                    reason = 5;
                Buf b;
                head(s, b, kSwitch, 0, static_cast<uint16_t>(reason));
                b.u32(static_cast<uint32_t>(s.ranAny ? s.lastThread : 0));
                b.u32(s.ranAny ? s.lastDepth : 0);
                b.u32(static_cast<uint32_t>(thread));
                b.u32(depth);
                b.u32(waitReason);
                b.u32(waitObject);
                b.u64(token);
                finishRecord(b, 0);
                emit(s, b);
            }
            // The context's changes since guest code last ran on it.
            uint32_t now[kCtxWords];
            toRecord(context, now);
            static const std::array<uint32_t, kCtxWords> kZero{};
            const auto it = s.seen.find(contextKey(thread, depth));
            const uint32_t *before = it != s.seen.end() ? it->second.data() : kZero.data();
            uint32_t mask[kMaskWords];
            std::vector<uint32_t> words;
            if (diffContext(before, now, mask, words) != 0)
            {
                Buf b;
                head(s, b, kCtx, 0, 0);
                b.u32(static_cast<uint32_t>(thread));
                b.u32(depth);
                b.u32(static_cast<uint32_t>(words.size()));
                b.u32(0);
                b.put(mask, sizeof(mask));
                b.put(words.data(), words.size() * 4);
                finishRecord(b, 0);
                if (s.mode == Mode::Check)
                {
                    ++s.records;
                    if (const uint8_t *want = compareRecord(s, b.b.data(), b.size()))
                    {
                        uint32_t rec[kCtxWords];
                        std::memcpy(rec, before, kCtxBytes);
                        const uint32_t count = rd32(want + 48);
                        const uint32_t *wantMask = reinterpret_cast<const uint32_t *>(want + 56);
                        (void)count;
                        applyContextWords(wantMask, wantMask + kMaskWords, rec);
                        fromRecord(rec, context);
                        ++s.substituted;
                    }
                }
                else
                    emit(s, b);
            }
        }
        catch (...)
        {
        }
        s.where = Where::Guest;
        s.synced = false;
    }

    void afterGuest(EeScheduler &scheduler, int thread, uint32_t depth) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        s.where = Where::Dispatcher;
        s.synced = false;
        s.ranAny = true;
        s.lastThread = thread;
        s.lastDepth = depth;
        if (GuestThread *t = scheduler.thread(thread))
        {
            const R5900Context *ctx = nullptr;
            if (depth == 0)
                ctx = &t->context;
            else if (depth <= t->invocations.size())
                ctx = &t->invocations[depth - 1].context;
            if (ctx)
            {
                auto &slot = s.seen[contextKey(thread, depth)];
                toRecord(*ctx, slot.data());
            }
        }
        if (s.strict)
            phaseBegin(kSrcDispatcher);
    }

    void invocationStarted(int thread, uint32_t depth, const GuestInvocation &invocation) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        s.seen.erase(contextKey(thread, depth));
        Buf b;
        head(s, b, kInvoke, 0, static_cast<uint16_t>(static_cast<uint32_t>(invocation.kind) + 1u));
        b.u32(static_cast<uint32_t>(thread));
        b.u32(depth);
        b.u32(invocation.context.pc);
        b.u32(invocation.onComplete ? 1u : 0u);
        b.u64(invocation.tag);
        b.u64(invocation.sequence);
        for (int i = 0; i < 4; ++i)
            b.u32(getRegU32(&invocation.context, 4 + i));
        b.u32(getRegU32(&invocation.context, 28));
        b.u32(getRegU32(&invocation.context, 29));
        b.u32(getRegU32(&invocation.context, 31));
        b.u32(0);
        finishRecord(b, 0);
        emit(s, b);
    }

    void invocationEnded(int thread, uint32_t depth, const GuestInvocation &invocation) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        s.seen.erase(contextKey(thread, depth));
        const auto it = s.invocationOrigin.find(invocation.sequence);
        s.completionOrigin = it != s.invocationOrigin.end() ? it->second : ~0ull;
        if (it != s.invocationOrigin.end())
            s.invocationOrigin.erase(it);
        Buf b;
        head(s, b, kInvokeEnd, 0, static_cast<uint16_t>(static_cast<uint32_t>(invocation.kind) + 1u));
        b.u32(static_cast<uint32_t>(thread));
        b.u32(depth);
        b.u32(getRegU32(&invocation.context, 2));
        b.u32(getRegU32(&invocation.context, 3));
        b.u64(invocation.sequence);
        finishRecord(b, 0);
        emit(s, b);
    }

    void op(uint32_t kind, const uint32_t *args, uint32_t count, uint64_t wide) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        try
        {
            Buf b;
            head(s, b, kOpRecord, 0, static_cast<uint16_t>(kind));
            const uint32_t origin = currentOrigin(s);
            b.u32(origin);
            b.u32(count);
            b.u64(origin == kSrcCall ? s.calls : ~0ull);
            b.u64(wide);
            for (uint32_t i = 0; i < count; ++i)
                b.u32(args[i]);
            finishRecord(b, 0);
            emit(s, b);
            if (kind == kOpBlock && count > 0 && origin == kSrcCall)
                s.waitOrigin[static_cast<int>(args[0])] = s.calls;
        }
        catch (...)
        {
        }
    }

    void opQueue(uint32_t kind, const GuestInvocation &invocation) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        const uint32_t args[] = {kind + 1u,
                                 invocation.context.pc,
                                 getRegU32(&invocation.context, 4),
                                 getRegU32(&invocation.context, 5),
                                 getRegU32(&invocation.context, 6),
                                 getRegU32(&invocation.context, 7),
                                 getRegU32(&invocation.context, 28),
                                 getRegU32(&invocation.context, 29),
                                 getRegU32(&invocation.context, 31),
                                 invocation.onComplete ? 1u : 0u,
                                 static_cast<uint32_t>(invocation.sequence),
                                 static_cast<uint32_t>(invocation.tag)};
        op(kOpQueue, args, 12, invocation.tag);
        if (currentOrigin(s) == kSrcCall)
            s.invocationOrigin[invocation.sequence] = s.calls;
    }

    void opBlock(int thread, const EeWaitState &wait) noexcept
    {
        uint32_t args[6] = {static_cast<uint32_t>(thread), static_cast<uint32_t>(wait.reason), 0, 0, 0,
                            wait.completion ? 1u : 0u};
        uint64_t wide = 0;
        if (const auto *sema = std::get_if<EeSemaphoreWait>(&wait.payload))
            args[2] = static_cast<uint32_t>(sema->id);
        else if (const auto *flag = std::get_if<EeEventFlagWait>(&wait.payload))
        {
            args[2] = static_cast<uint32_t>(flag->id);
            args[3] = flag->bits;
            args[4] = flag->mode;
            wide = flag->resultAddress;
        }
        else if (const auto *vsync = std::get_if<EeVSyncWait>(&wait.payload))
        {
            args[3] = static_cast<uint32_t>(vsync->fixedResult);
            wide = vsync->afterTick;
        }
        else if (const auto *external = std::get_if<EeExternalWait>(&wait.payload))
        {
            args[2] = external->type;
            wide = external->token;
        }
        op(kOpBlock, args, 6, wide);
    }

    void taskBegin() noexcept { ++state().taskDepth; }
    void taskEnd() noexcept
    {
        State &s = state();
        if (s.taskDepth)
            --s.taskDepth;
    }

    void iopTouch() noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound || s.call.open || s.iopOpen)
            return;
        if (s.where == Where::Dispatcher && s.source != kSrcNone)
            flushWindow(s, s.source);
        ensureSynced(s);
        s.iopOpen = true;
    }

    void iopBegin() noexcept { iopTouch(); }

    void iopEnd(uint32_t site) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.iopOpen)
            return;
        s.iopOpen = false;
        if (s.call.open)
            return;
        flushWindow(s, site == 2 ? kSrcIopVblank : kSrcIop);
        if (s.where != Where::Dispatcher || s.source == kSrcNone)
            s.synced = false;
    }

    void frameEnd() noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return;
        if (s.call.open)
            s.call.flags |= kCallFrameInside;
        Buf b;
        head(s, b, kFrame, 0, 0);
        b.u32(static_cast<uint32_t>(ps2x::det::frame()));
        b.u32(0);
        finishRecord(b, 0);
        emit(s, b);
        if (!s.call.open && s.chunk.size() >= kChunkRawBytes / 4u)
            flushChunk(s);
        const uint64_t frame = ps2x::det::frame();
        if (frame % 300u == 0u)
        {
            if (s.out)
                std::fflush(s.out);
            std::fprintf(stderr, "[calllog] frame %" PRIu64 ": %" PRIu64 " calls, %" PRIu64 " records, %.1f MB\n", frame,
                         s.calls, s.records, (s.bytes + s.chunk.size()) / 1048576.0);
        }
    }

    void finish() noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off)
            return;
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;
        // The run ends inside a call (the reference's frame limit ends it at a frame end): its record as it stands.
        if (s.call.open)
        {
            s.call.flags |= kCallCut;
            const uint32_t token = s.call.id;
            const int saved = s.call.uncaught;
            s.call.uncaught = std::uncaught_exceptions();
            callLeave(token);
            (void)saved;
        }
        if (s.mode == Mode::Check)
        {
            const uint8_t *left = s.expected.peek();
            char line[600];
            std::snprintf(line, sizeof(line),
                          "%" PRIu64 " records compared: %" PRIu64 " identical, %" PRIu64
                          " differ (CALL %" PRIu64 ", MEM %" PRIu64 ", CTX %" PRIu64 ", other %" PRIu64 ")%s; %" PRIu64
                          " windows substituted with the log's effects; log %s",
                          s.compared, s.matched, s.differ, s.differByType[kCall], s.differByType[kMem],
                          s.differByType[kCtx],
                          s.differ - s.differByType[kCall] - s.differByType[kMem] - s.differByType[kCtx],
                          s.desynced ? ", DESYNCHRONIZED" : "", s.substituted,
                          left ? "has records left" : "fully consumed");
            s.report += line;
            s.report += "\n";
            std::fprintf(stderr, "[calllog-check] %s\n", line);
            if (FILE *f = std::fopen((s.path + ".check.txt").c_str(), "w"))
            {
                std::fputs(s.report.c_str(), f);
                std::fclose(f);
            }
            std::fflush(stderr);
            return;
        }
        if (!s.out)
            return;
        flushChunk(s);
        Buf b;
        b.u64(s.records);
        b.u64(s.calls);
        b.u32(static_cast<uint32_t>(ps2x::det::frame()));
        b.u32(0);
        b.u64(s.scheduler ? s.scheduler->currentCycle() : 0u);
        writeChunk(s, kTagEnd, b, false);
        std::fclose(s.out);
        s.out = nullptr;
        writeSummary(s);
        std::fprintf(stderr, "[calllog] done: %" PRIu64 " records, %" PRIu64 " calls, %.1f MB in %s\n", s.records,
                     s.calls, s.bytes / 1048576.0, s.path.c_str());
        std::fflush(stderr);
    }
}
