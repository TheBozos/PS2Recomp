// The call log, its self-check and its replay (runtime/ps2_calllog.h). The file format is BozoEngine's
// docs/specs/calllog.md ("PS2XCLG1").

#include "runtime/ps2_calllog.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
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
    bool g_replay = false;

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
            kSrcNone = 0,         // OP records: an EE checkpoint inside guest code
            kSrcCall = 1,         // inside an out-call (OP records only: the call's own effects are in its record)
            kSrcIop = 2,          // an IOP slice run as EE cycles were accounted
            kSrcIopVblank = 3,    // the IOP's catch-up at a vblank
            kSrcEvents = 4,       // the scheduler's events: vblank, timers, alarms, posted tasks
            kSrcResume = 5,       // a wait's completion as its thread resumes
            kSrcHandlerEnd = 6,   // a handler's completion as it returns
            kSrcHandlerStart = 7, // a handler set up on a thread
            kSrcThreadEnd = 8,    // a thread's end (its observers)
            kSrcDispatcher = 9,   // the rest of the dispatcher's work
            kSrcTask = 10,        // a posted task running (OP records only; its memory is in the events')
        };

        enum CallFlags : uint8_t
        {
            kCallUnwound = 1u << 0,     // left through the dispatcher (the thread blocked or switched, a handler began)
            kCallFrameInside = 1u << 1, // the game frame ended inside the call (a FRAME record follows it)
            kCallCut = 1u << 2,         // the run ended inside the call
            kCallContextLost = 1u << 3, // unwound and its context no longer exists: no context words
            kCallTransfer = 1u << 4,    // returned with the dispatcher transfer set: the code after it unwinds
        };
        // Any record but CALL: made while the CALL record before it was open (its seq is that call's).
        constexpr uint8_t kInsideCall = 0x80u;

        enum class Mode
        {
            Off,
            Record,
            Check,
            Replay,
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
            Buf rec;          // the record, its out-call part completed at the end
            size_t outAt = 0; // where the out-call part starts
            int thread = 0;
            size_t traceFrom = 0;               // the reference's traced calls of the frame before this call's
            uint32_t ownTrace = 0;              // an override's address or a syscall's id: its own traced entry, which
                                                // the reference pushes after the call opened (skipped)
            std::vector<uint32_t> earlierPages; // pages written before a frame end inside it
        };

        // The log read back (the self-check and the replay).
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
            // The records made inside the call at the cursor (it and they share a chunk: the recorder writes a call's
            // records together), as offsets into raw.
            std::vector<size_t> insideCall()
            {
                std::vector<size_t> out;
                const uint8_t *p = peek();
                if (!p)
                    return out;
                size_t off = at + rd32(p + 4);
                while (off + kRecordHeaderBytes <= raw.size() && raw[off] != kCall && (raw[off + 1] & kInsideCall))
                {
                    out.push_back(off);
                    off += rd32(raw.data() + off + 4);
                }
                return out;
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
            uint32_t opDepth = 0;

            uint64_t calls = 0;
            uint32_t checkpoints = 0;
            uint64_t records = 0;
            OpenCall call;
            uint32_t nextId = 1;
            Buf pending; // records made while a call is open: written after it
            Buf chunk;
            FILE *out = nullptr;
            uint64_t bytes = 0;
            std::vector<std::pair<uint32_t, std::string>> names;
            std::unordered_map<uint32_t, uint32_t> classes; // stub address -> 1 kernel, 2 library

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
            uint64_t typeCounts[16] = {};
            std::map<uint32_t, uint64_t> callsByTarget;

            // The self-check and the replay.
            Expected expected;
            bool desynced = false;
            bool answering = false; // the records of a call answered from the log (their pc and checkpoints are not
                                    // the replay's)
            uint64_t compared = 0, matched = 0, differ = 0, substituted = 0;
            uint64_t differByType[16] = {};
            uint64_t answered = 0, native = 0, reissued = 0, appliedAsync = 0, logSupplied = 0;
            // The replay: invocations it queued again (their sequence numbers), and the (thread, depth) they run at:
            // their whole first context comes from the log (the library built it from state the replay never made).
            bool reissuing = false;
            bool expectedDifference = false;
            std::unordered_set<uint64_t> replayInvocations;
            std::unordered_set<uint64_t> logContexts;
            const uint8_t *replayFrame = nullptr; // the FRAME record made again inside an answered call
            std::map<std::string, uint64_t> differences;
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

        // What memory changed since the shadow was synced, as runs.
        uint32_t diffRuns(State &s, Buf &out)
        {
            collect(s);
            uint32_t runs = 0;
            for (uint32_t p : s.pages)
                runs += diffPage(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), p * kPage, out);
            return runs;
        }

        // The reference's traced calls made since `from` (the calls field of PS2XREF1), as count then 32-byte entries.
        uint32_t putTraces(Buf &b, size_t from, uint32_t &own)
        {
            std::vector<ps2x::reference::TracedCall> traces;
            ps2x::reference::tracedCallsSince(from, traces);
            // The call's own entry (the dispatcher records an override, the runtime a syscall, as the call begins,
            // after the call log opened it).
            if (own != 0 && !traces.empty() &&
                ((own & 0x80000000u) ? traces.front().id == own : traces.front().pc == (own & (PS2_RAM_SIZE - 1u))))
                traces.erase(traces.begin());
            own = 0;
            b.u32(static_cast<uint32_t>(traces.size()));
            for (const auto &t : traces)
            {
                b.u32(t.id);
                b.u32(t.pc);
                for (int i = 0; i < 4; ++i)
                    b.u32(t.a[i]);
                b.u32(t.v0);
                b.u32(t.flags);
            }
            return static_cast<uint32_t>(traces.size());
        }

        void injectTraces(const uint8_t *p, uint32_t count)
        {
            std::vector<ps2x::reference::TracedCall> traces(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t *e = p + 32u * i;
                traces[i] = ps2x::reference::TracedCall{rd32(e), rd32(e + 4),
                                                        {rd32(e + 8), rd32(e + 12), rd32(e + 16), rd32(e + 20)},
                                                        rd32(e + 24), rd32(e + 28)};
            }
            ps2x::reference::injectTracedCalls(traces.data(), traces.size());
        }

        // The pages a call wrote: the ones collected now and the ones before a frame end inside it.
        void mergeCallPages(State &s)
        {
            if (s.call.earlierPages.empty())
                return;
            s.pages.insert(s.pages.end(), s.call.earlierPages.begin(), s.call.earlierPages.end());
            std::sort(s.pages.begin(), s.pages.end());
            s.pages.erase(std::unique(s.pages.begin(), s.pages.end()), s.pages.end());
            s.call.earlierPages.clear();
        }

        void shadowCatchUp(State &s)
        {
            for (uint32_t p : s.pages)
                std::memcpy(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), kPage);
        }

        void applyRuns(State &s, const uint8_t *runs, uint32_t count)
        {
            const uint8_t *q = runs;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t address = rd32(q), len = rd32(q + 4);
                if (address + len <= kFlatBytes)
                    for (uint32_t j = 0; j < len; ++j)
                        *flatByte(s, address + j) = q[8 + j];
                q += 8 + ((len + 3u) & ~3u);
            }
        }

        // The run's own writes to the collected pages thrown away (back to the shadow), the log's runs applied.
        void substituteRuns(State &s, const uint8_t *runs, uint32_t count)
        {
            for (uint32_t p : s.pages)
                std::memcpy(flatPage(s, p), s.shadow.data() + size_t(p) * kPage, kPage);
            applyRuns(s, runs, count);
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
            const auto &overrides = ps2x::capture::overrideAddresses();
            for (const auto &[address, name] : s.names)
            {
                b.u32(address);
                uint32_t kind = s.classes.count(address) ? s.classes[address] : 2u;
                if (std::find(overrides.begin(), overrides.end(), address) != overrides.end())
                    kind = 3u;
                b.u32(kind);
                b.u32(static_cast<uint32_t>(name.size()));
                b.put(name.data(), name.size());
                b.pad4();
            }
            writeChunk(s, kTagNames, b, true);
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
            std::snprintf(text, sizeof(text), "%s sub %u seq %" PRIu64 " frame %u cycle %" PRIu64 " thread %d pc 0x%x",
                          r[0] < 10 ? types[r[0]] : "?", static_cast<unsigned>(r[2] | (r[3] << 8)), rd64(r + 16),
                          rd32(r + 24), rd64(r + 8), static_cast<int>(rd32(r + 28)), rd32(r + 36));
            std::string out = text;
            if (r[0] == kCall)
            {
                std::snprintf(text, sizeof(text), " kind %u target 0x%x a0 0x%x", r[96], rd32(r + 100), rd32(r + 104));
                out += text;
            }
            return out;
        }

        void note(State &s, const std::string &line)
        {
            s.report += line + "\n";
            if (s.logged++ < 30)
                std::fprintf(stderr, "[calllog-%s] %s\n", s.mode == Mode::Replay ? "replay" : "check", line.c_str());
        }

        void writeReport(State &s);

        // The replay cannot go on once the run and the log part ways.
        [[noreturn]] void diverge(State &s, const std::string &why)
        {
            note(s, "DIVERGED: " + why);
            s.desynced = true;
            writeReport(s);
            std::fflush(nullptr);
            std::_Exit(3);
        }

        // The self-check and the replay: the record the run made, compared with the log's next one (consumed).
        // Returns the log's record when they are the same record (same type, point and target), whatever their
        // contents, else nullptr. lenient: the record was made by a call answered from the log, whose pc and
        // checkpoints at the time are the reference's own.
        const uint8_t *compareRecord(State &s, const uint8_t *made, size_t bytes, bool lenient = false)
        {
            if (s.desynced)
                return nullptr;
            const uint8_t *want = s.expected.peek();
            ++s.compared;
            if (!want)
            {
                if (s.mode == Mode::Replay)
                    diverge(s, "the log ended before " + describe(made));
                note(s, "the log ended before " + describe(made));
                s.desynced = true;
                return nullptr;
            }
            const uint32_t wantBytes = rd32(want + 4);
            const bool sameRecord = want[0] == made[0] && std::memcmp(want + 2, made + 2, 2) == 0 &&
                                    std::memcmp(want + 8, made + 8, lenient ? 24 : 32) == 0 &&
                                    (made[0] != kCall || std::memcmp(want + 96, made + 96, 24) == 0);
            if (!sameRecord)
            {
                if (s.mode == Mode::Replay)
                    diverge(s, "the run made " + describe(made) + ", the log has " + describe(want));
                note(s, "desynchronized: the run made " + describe(made) + ", the log has " + describe(want));
                s.desynced = true;
                return nullptr;
            }
            s.expected.next();
            bool same = wantBytes == bytes;
            if (same && lenient)
                same = std::memcmp(want, made, 32) == 0 && std::memcmp(want + 40, made + 40, bytes - 40) == 0;
            else if (same)
                same = std::memcmp(want, made, bytes) == 0;
            if (same)
                ++s.matched;
            else if (s.expectedDifference)
            {
                ++s.matched;
                ++s.logSupplied;
            }
            else
            {
                ++s.differ;
                ++s.differByType[made[0] & 15u];
                size_t at = 0;
                const size_t common = std::min<size_t>(bytes, wantBytes);
                while (at < common && (want[at] == made[at] || (lenient && at >= 32 && at < 40)))
                    ++at;
                char text[160];
                std::snprintf(text, sizeof(text), " (first difference at byte %zu: run 0x%02x, log 0x%02x; %zu and %u bytes)",
                              at, at < bytes ? made[at] : 0u, at < wantBytes ? want[at] : 0u, bytes, wantBytes);
                note(s, "contents differ: " + describe(made) + text);
            }
            s.expectedDifference = false;
            return want;
        }

        void emitPending(State &s);

        // A finished record: to the file (or compared). Records made while a call is open wait for its record.
        void emit(State &s, Buf &b)
        {
            ++s.records;
            if (s.call.open)
            {
                b.b[1] |= kInsideCall;
                s.pending.put(b.b.data(), b.size());
                return;
            }
            ++s.typeCounts[b.b[0] & 15u];
            if (s.mode != Mode::Record)
            {
                (void)compareRecord(s, b.b.data(), b.size());
                return;
            }
            s.chunk.put(b.b.data(), b.size());
            if (s.chunk.size() >= kChunkRawBytes)
                flushChunk(s);
        }

        void emitPending(State &s)
        {
            if (s.pending.size() == 0)
                return;
            Buf records;
            std::swap(records, s.pending);
            const uint8_t *p = records.b.data();
            const uint8_t *end = p + records.size();
            while (p < end)
            {
                const uint32_t bytes = rd32(p + 4);
                ++s.typeCounts[p[0] & 15u];
                if (s.mode != Mode::Record)
                    (void)compareRecord(s, p, bytes, s.answering);
                else
                    s.chunk.put(p, bytes);
                p += bytes;
            }
            if (s.chunk.size() >= kChunkRawBytes)
                flushChunk(s);
        }

        // ---- Windows --------------------------------------------------------------------------------------------------
        // The changes made since the shadow was synced, as a MEM record of the source. In the self-check and the
        // replay, the log's record for this point replaces them.
        void flushWindow(State &s, uint32_t source)
        {
            if (!s.synced)
                return;
            Buf runs;
            const uint32_t count = diffRuns(s, runs);
            bool expected = false;
            if (s.mode != Mode::Record && !s.desynced)
            {
                const uint8_t *want = s.expected.peek();
                expected = want && want[0] == kMem && (want[2] | (want[3] << 8)) == source &&
                           rd64(want + 8) == s.scheduler->currentCycle() && rd64(want + 16) == s.calls;
            }
            if (count == 0 && !expected)
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
            if (s.mode != Mode::Record)
            {
                ++s.records;
                ++s.typeCounts[kMem];
                // The replay makes no library continuation: their memory comes from the log.
                s.expectedDifference = s.mode == Mode::Replay && (source == kSrcResume || source == kSrcHandlerEnd ||
                                                                  source == kSrcThreadEnd);
                if (const uint8_t *want = compareRecord(s, b.b.data(), b.size()))
                    substituteRuns(s, want + 56, rd32(want + 40));
                shadowCatchUp(s);
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
            std::fprintf(f, "# calls by target: kind target count name\n");
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

        void writeReport(State &s)
        {
            const uint8_t *left = s.desynced ? nullptr : s.expected.peek();
            char line[900];
            std::snprintf(line, sizeof(line),
                          "%" PRIu64 " records compared: %" PRIu64 " identical, %" PRIu64
                          " differ (CALL %" PRIu64 ", MEM %" PRIu64 ", CTX %" PRIu64 ", other %" PRIu64 ")%s; %" PRIu64
                          " windows' effects taken from the log; log %s",
                          s.compared, s.matched, s.differ, s.differByType[kCall], s.differByType[kMem],
                          s.differByType[kCtx],
                          s.differ - s.differByType[kCall] - s.differByType[kMem] - s.differByType[kCtx],
                          s.desynced ? ", DESYNCHRONIZED" : "", s.substituted,
                          s.desynced ? "not followed to its end" : (left ? "has records left" : "fully consumed"));
            std::string text = s.report + line + "\n";
            if (s.mode == Mode::Replay)
            {
                std::snprintf(line, sizeof(line),
                              "replay: %" PRIu64 " calls answered from the log, %" PRIu64
                              " made by this runtime's kernel; %" PRIu64 " kernel operations of libraries, overrides "
                              "and continuations made again; %" PRIu64 " asynchronous records applied; %" PRIu64
                              " records whose contents only the log has (handlers' first contexts, library "
                              "continuations' memory and contexts); frame %" PRIu64
                              "\n",
                              s.answered, s.native, s.reissued, s.appliedAsync, s.logSupplied,
                              static_cast<uint64_t>(ps2x::det::frame()));
                text += line;
            }
            std::fprintf(stderr, "[calllog-%s] %s", s.mode == Mode::Replay ? "replay" : "check",
                         text.substr(s.report.size()).c_str());
            const std::string path = s.path + (s.mode == Mode::Replay ? ".replay.txt" : ".check.txt");
            if (FILE *f = std::fopen(path.c_str(), "w"))
            {
                std::fputs(text.c_str(), f);
                std::fclose(f);
            }
        }

        // ---- The replay -----------------------------------------------------------------------------------------------
        bool kernelCall(State &s, uint32_t kind, uint32_t target)
        {
            if (kind == capture::kOutSyscall)
                return true;
            if (kind != capture::kOutStub)
                return false;
            const auto it = s.classes.find(target & (PS2_RAM_SIZE - 1u));
            return it != s.classes.end() && it->second == 1u;
        }

        void replayContinuation(uint32_t origin);

        void replayResume(R5900Context &)
        {
            replayContinuation(kSrcResume);
        }

        void replayHandlerEnd(const R5900Context &, R5900Context &)
        {
            replayContinuation(kSrcHandlerEnd);
        }

        void replayTask()
        {
            replayContinuation(kSrcTask);
        }

        GuestInvocation invocationOf(uint32_t kind, uint32_t pc, const uint32_t *a, uint32_t gp, uint32_t sp, uint32_t ra,
                                     uint64_t tag, bool completion)
        {
            GuestInvocation invocation{};
            invocation.kind = static_cast<GuestInvocationKind>(kind);
            invocation.tag = tag;
            invocation.context.pc = pc;
            for (int i = 0; i < 4; ++i)
                SET_GPR_U32(&invocation.context, 4 + i, a[i]);
            SET_GPR_U32(&invocation.context, 28, gp);
            SET_GPR_U32(&invocation.context, 29, sp);
            SET_GPR_U32(&invocation.context, 31, ra);
            if (completion)
                invocation.onComplete = &replayHandlerEnd;
            return invocation;
        }

        // An operation a library, an override, a continuation or the IOP made on the kernel, made again.
        void reissue(State &s, const uint8_t *r)
        {
            EeScheduler &ee = *s.scheduler;
            const uint32_t op = r[2] | (r[3] << 8);
            const uint32_t count = rd32(r + 44);
            const uint64_t wide = rd64(r + 64);
            uint32_t a[16] = {};
            for (uint32_t i = 0; i < count && i < 16; ++i)
                a[i] = rd32(r + 72 + 4 * i);
            ++s.reissued;
            s.reissuing = true;
            struct Reset
            {
                bool &flag;
                ~Reset() { flag = false; }
            } reset{s.reissuing};
            int old = 0;
            uint32_t owned = 0;
            switch (op)
            {
            case kOpQueue:
                ee.queueInvocation(invocationOf(a[0] - 1u, a[1], a + 2, a[6], a[7], a[8], wide, a[9] != 0));
                break;
            case kOpBlock:
            {
                EeWaitState wait{};
                wait.reason = static_cast<EeWaitReason>(a[1]);
                switch (wait.reason)
                {
                case EeWaitReason::Semaphore:
                    wait.payload = EeSemaphoreWait{static_cast<int>(a[2])};
                    break;
                case EeWaitReason::EventFlag:
                    wait.payload = EeEventFlagWait{static_cast<int>(a[2]), a[3], a[4], static_cast<uint32_t>(wide)};
                    break;
                case EeWaitReason::VSync:
                    wait.payload = EeVSyncWait{wide, static_cast<int>(a[3])};
                    break;
                case EeWaitReason::External:
                case EeWaitReason::Mpeg:
                    wait.payload = EeExternalWait{a[2], wide};
                    break;
                default:
                    break;
                }
                if (a[5])
                    wait.completion = &replayResume;
                ee.replayBlock(std::move(wait));
            }
            case kOpWake:
                ee.replayWake(static_cast<int>(a[0]), static_cast<int>(a[1]));
                break;
            case kOpIrq:
                ee.dispatchIrq(a[0] != 0, a[1]);
                break;
            case kOpTask:
                ee.postTask(&replayTask);
                break;
            case kOpEvent:
                ee.postEvent(EeEvent{static_cast<EeEventType>(a[0]), a[1], wide});
                break;
            case kOpCreate:
            {
                EeThreadCreateParams params{};
                params.entry = a[1];
                params.stack = a[2];
                params.stackSize = a[3];
                params.priority = static_cast<int>(a[4]);
                params.gp = a[5];
                params.attr = a[6];
                params.option = a[7];
                (void)ee.createThread(params);
                break;
            }
            case kOpStart:
            {
                const R5900Context *caller = ee.currentContext();
                R5900Context empty{};
                (void)ee.startThread(static_cast<int>(a[0]), a[1], caller ? *caller : empty, a[2] != 0);
                break;
            }
            case kOpExit:
                ee.exitCurrent(a[1] != 0);
            case kOpTerminate:
                if (a[1] == 1u)
                    (void)ee.deleteThread(static_cast<int>(a[0]), owned);
                else
                    (void)ee.terminateThread(static_cast<int>(a[0]), owned, a[2] != 0);
                break;
            case 11:
                (void)ee.changePriority(static_cast<int>(a[0]), static_cast<int>(a[1]), a[2] != 0, old);
                break;
            case 12:
                (void)ee.rotateReadyQueue(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 13:
                (void)ee.suspendThread(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 14:
                (void)ee.resumeThread(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 15:
                (void)ee.wakeupThread(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 16:
                (void)ee.cancelWakeup(static_cast<int>(a[0]));
                break;
            case 17:
                (void)ee.releaseWait(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 18:
                (void)ee.createSemaphore(static_cast<int>(a[0]), static_cast<int>(a[1]), a[2], a[3]);
                break;
            case 19:
                (void)ee.deleteSemaphore(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 20:
                (void)ee.signalSemaphore(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 21:
                (void)ee.pollSemaphore(static_cast<int>(a[0]));
                break;
            case 22:
                ee.waitSemaphore(static_cast<int>(a[0]));
                break;
            case 23:
                (void)ee.createEventFlag(a[0], a[1], a[2]);
                break;
            case 24:
                (void)ee.deleteEventFlag(static_cast<int>(a[0]), a[1] != 0);
                break;
            case 25:
                (void)ee.setEventFlag(static_cast<int>(a[0]), a[1], a[2] != 0);
                break;
            case 26:
                (void)ee.clearEventFlag(static_cast<int>(a[0]), a[1]);
                break;
            case 27:
            {
                uint32_t observed = 0;
                (void)ee.pollEventFlag(static_cast<int>(a[0]), a[1], a[2], observed);
                break;
            }
            case 28:
                ee.waitEventFlag(static_cast<int>(a[0]), a[1], a[2], a[3]);
                break;
            case 29:
                (void)ee.setAlarm(static_cast<uint16_t>(a[0]), a[1], a[2], a[3], a[4]);
                break;
            case 30:
                (void)ee.cancelAlarm(static_cast<int>(a[0]));
                break;
            case 31:
                (void)ee.addIrqHandler(a[0] != 0, a[1], a[2], a[3] != 0, a[4], a[5], a[6]);
                break;
            case 32:
                (void)ee.removeIrqHandler(a[0] != 0, a[1], static_cast<int>(a[2]));
                break;
            case 33:
                (void)ee.setIrqHandlerEnabled(a[0] != 0, static_cast<int>(a[1]), a[2] != 0);
                break;
            case 34:
                (void)ee.setIrqCauseEnabled(a[0] != 0, a[1], a[2] != 0);
                break;
            case 35:
                ee.setVSyncFlag(a[0], a[1]);
                break;
            case 36:
                (void)ee.setGsVSyncCallback(a[0], a[1], a[2]);
                break;
            case 37:
                ee.setFieldRate(EeScheduler::FieldRate{a[0], a[1]});
                break;
            case 38:
                ee.setupCurrentThread(a[0], a[1], a[2]);
                break;
            case 39:
                ee.transferIfRequested(a[0] != 0);
                break;
            default:
                diverge(s, "an operation the replay cannot make: " + describe(r));
            }
        }

        // A continuation (a wait's, a handler's, a posted task's) of a library: its operations on the kernel, made
        // again in order, up to the first record that is not one of them.
        void replayContinuationImpl(State &s, uint32_t origin)
        {
            for (;;)
            {
                const uint8_t *want = s.expected.peek();
                if (!want || want[0] != kOpRecord || (want[1] & kInsideCall) || rd32(want + 40) != origin)
                    return;
                if (rd32(want + 48) != 0)
                {
                    note(s, "an operation made inside another was not made again: " + describe(want));
                    s.expected.next();
                    continue;
                }
                reissue(s, want);
            }
        }

        void replayContinuation(uint32_t origin)
        {
            State &s = state();
            if (s.mode == Mode::Replay && !s.desynced)
                replayContinuationImpl(s, origin);
        }

        // The IOP's records at this EE cycle (the replay runs no IOP): its writes applied, its operations made again.
        void replayIop(State &s, uint32_t site)
        {
            for (;;)
            {
                const uint8_t *want = s.expected.peek();
                if (!want || (want[1] & kInsideCall) || rd64(want + 8) != s.scheduler->currentCycle() ||
                    rd64(want + 16) != s.calls)
                    return;
                if (want[0] == kMem && ((want[2] | (want[3] << 8)) == (site == 2 ? kSrcIopVblank : kSrcIop)))
                {
                    applyRuns(s, want + 56, rd32(want + 40));
                    ++s.appliedAsync;
                    ++s.compared;
                    ++s.matched;
                    s.expected.next();
                    continue;
                }
                if (want[0] == kOpRecord && rd32(want + 40) == kSrcIop)
                {
                    s.iopOpen = true;
                    try
                    {
                        if (rd32(want + 48) == 0)
                            reissue(s, want);
                        else
                        {
                            note(s, "an operation made inside another was not made again: " + describe(want));
                            s.expected.next();
                        }
                    }
                    catch (...)
                    {
                        s.iopOpen = false;
                        throw;
                    }
                    s.iopOpen = false;
                    ++s.appliedAsync;
                    continue;
                }
                return;
            }
        }

        // A call answered from the log: its context words and memory runs applied, its time taken, the operations it
        // made on the kernel and the frame end inside it made again; the call itself is not made.
        void answerCall(State &s, R5900Context *ctx, void *ioValue, uint32_t ioSize)
        {
            const uint8_t *want = s.expected.peek();
            const std::vector<uint8_t> call(want, want + rd32(want + 4));
            const std::vector<size_t> inside = s.expected.insideCall();
            std::vector<std::vector<uint8_t>> insideRecords;
            for (size_t off : inside)
                insideRecords.emplace_back(s.expected.raw.begin() + off,
                                           s.expected.raw.begin() + off + rd32(s.expected.raw.data() + off + 4));
            const uint8_t *r = call.data();
            const uint8_t *out = r + 96;
            const uint32_t ctxCount = rd32(out + 24);
            const uint32_t runCount = rd32(out + 28);
            const uint32_t *mask = reinterpret_cast<const uint32_t *>(out + kOutHeaderBytes);
            const uint32_t *words = mask + kMaskWords;
            const uint8_t *runs = reinterpret_cast<const uint8_t *>(words + ctxCount);

            OpenCall &c = s.call;
            c.open = true;
            c.id = s.nextId++;
            c.prev = s.where;
            c.flags = r[1];
            c.ctx = ctx;
            c.thread = s.scheduler->currentThreadId();
            s.where = Where::Call;
            s.answering = true;
            ++s.answered;
            uint32_t entry[kCtxWords];
            toRecord(*ctx, entry);
            // The context as the call left it (again after the operations made again, which may set the caller's
            // return registers as the kernel does).
            const auto applyContext = [&]()
            {
                if (r[1] & kCallContextLost)
                    return;
                uint32_t rec[kCtxWords];
                std::memcpy(rec, entry, kCtxBytes);
                applyContextWords(mask, words, rec);
                if (s.scheduler->ownsContext(ctx))
                    fromRecord(rec, *ctx);
            };
            applyContext();
            const bool frameInside = (r[1] & kCallFrameInside) != 0;
            const uint8_t *traces = skipRuns(runs, runCount);
            if (!frameInside)
                applyRuns(s, runs, runCount);
            bool ended = false;
            // The call's effects that follow a frame end inside it, and the calls it made (the frame's call list).
            const auto endEffects = [&]()
            {
                if (ended)
                    return;
                ended = true;
                applyContext();
                if (frameInside)
                    applyRuns(s, runs, runCount);
                injectTraces(traces + 4, rd32(traces));
            };
            if (ioValue)
                std::memcpy(ioValue, out + 8, std::min<uint32_t>(ioSize, 16u));

            const auto close = [&]()
            {
                c.open = false;
                ++s.compared;
                ++s.matched;
                s.expected.next(); // the CALL itself
                ++s.calls;
                s.checkpoints = 0;
                emitPending(s);
                s.answering = false;
                s.where = c.prev;
                if (s.where == Where::Guest)
                    s.synced = false;
            };
            try
            {
                for (const auto &rec : insideRecords)
                {
                    const uint8_t *p = rec.data();
                    if (p[0] == kOpRecord)
                    {
                        if (rd32(p + 48) != 0)
                            continue; // made by the kernel inside an operation made again
                        s.scheduler->replayAdvanceTo(rd64(p + 8));
                        reissue(s, p);
                    }
                    else if (p[0] == kInvoke)
                    {
                        s.scheduler->replayAdvanceTo(rd64(p + 8));
                        const uint32_t a[4] = {rd32(p + 72), rd32(p + 76), rd32(p + 80), rd32(p + 84)};
                        s.scheduler->invokeCurrent(invocationOf((p[2] | (p[3] << 8)) - 1u, rd32(p + 48), a,
                                                                rd32(p + 88), rd32(p + 92), rd32(p + 96), rd64(p + 56),
                                                                (rd32(p + 52) & 1u) != 0));
                    }
                    else if (p[0] == kFrame)
                    {
                        s.scheduler->replayAdvanceTo(rd64(p + 8));
                        const uint32_t frameRuns = rd32(p + 44);
                        applyRuns(s, p + 56, frameRuns);
                        injectTraces(skipRuns(p + 56, frameRuns), rd32(p + 48));
                        s.replayFrame = p;
                        s.scheduler->presentGuestFrame();
                        s.replayFrame = nullptr;
                    }
                }
                endEffects();
                s.scheduler->replayAdvanceTo(rd64(r + 72));
                s.runtime->memory().setTimerSpinCycles(rd32(r + 84));
                if (r[1] & kCallTransfer)
                    s.scheduler->noteTransferToDispatcher();
            }
            catch (const EeDispatcherTransfer &)
            {
                endEffects();
                close();
                throw;
            }
            close();
            if (r[1] & kCallUnwound)
            {
                // It left through the dispatcher in the reference, and nothing made again did here.
                note(s, "a call that left through the dispatcher did not here: " + describe(r));
                s.scheduler->noteTransferToDispatcher();
                throw EeDispatcherTransfer{};
            }
        }

        bool matchCall(State &s, const uint8_t *want, capture::OutKind kind, uint32_t target, R5900Context *ctx,
                       uint32_t size, const void *value)
        {
            if (!want || want[0] != kCall || want[96] != kind || rd32(want + 100) != target)
                return false;
            if (rd64(want + 8) != s.scheduler->currentCycle() || rd64(want + 16) != s.calls ||
                rd32(want + 28) != static_cast<uint32_t>(s.scheduler->currentThreadId()))
                return false;
            if (kind == capture::kOutStub || kind == capture::kOutOverride)
            {
                for (int i = 0; i < 4; ++i)
                    if (rd32(want + 104 + 4 * i) != getRegU32(ctx, 4 + i))
                        return false;
            }
            else if (kind == capture::kOutSyscall)
            {
                if (rd32(want + 104) != getRegU32(ctx, 3))
                    return false;
            }
            else if (kind == capture::kOutIoLoad || kind == capture::kOutIoStore)
            {
                if (want[97] != size)
                    return false;
                if (kind == capture::kOutIoStore && std::memcmp(want + 104, value, std::min<uint32_t>(size, 16u)) != 0)
                    return false;
            }
            return true;
        }

        // The replay at an out-call: the log's next record must be this call; the kernel's are made, the others
        // answered. Returns true when answered.
        bool replayCall(State &s, R5900Context *ctx, capture::OutKind kind, uint32_t target, uint32_t size, void *value,
                        bool &native)
        {
            native = false;
            const uint8_t *want = s.expected.peek();
            if (!matchCall(s, want, kind, target, ctx, size, value))
            {
                char text[160];
                std::snprintf(text, sizeof(text), "the run calls kind %u target 0x%x a0 0x%x at cycle %" PRIu64
                                                  " seq %" PRIu64 ", the log has ",
                              kind, target, getRegU32(ctx, 4), s.scheduler->currentCycle(), s.calls);
                diverge(s, text + (want ? describe(want) : std::string("nothing")));
            }
            if (kernelCall(s, kind, target))
            {
                native = true;
                ++s.native;
                return false;
            }
            answerCall(s, ctx, kind == capture::kOutIoLoad ? value : nullptr, size);
            return true;
        }

        bool callEnterImpl(State &s, R5900Context *ctx, PS2Runtime *runtime, capture::OutKind kind, uint32_t target,
                           uint32_t &token)
        {
            OpenCall &c = s.call;
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
            c.traceFrom = ps2x::reference::tracedCallCount();
            c.ownTrace = kind == capture::kOutOverride ? target
                         : kind == capture::kOutSyscall ? 0x80000000u | (getRegU32(ctx, 3) & 0x7FFFFFFFu)
                                                        : 0u;
            c.earlierPages.clear();
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
        const char *replay = std::getenv("PS2X_CALL_LOG_REPLAY");
        const char *against = replay && *replay ? replay : check;
        if ((record && *record) || (against && *against))
        {
            if (!ps2x::det::enabled())
            {
                std::fprintf(stderr, "[calllog] needs PS2X_DETERMINISTIC=1: off\n");
                return false;
            }
            if (const char *v = std::getenv("PS2X_CALL_LOG_STRICT"); v && *v == '1')
                s.strict = true;
            if (against && *against)
            {
                s.mode = (replay && *replay) ? Mode::Replay : Mode::Check;
                s.path = against;
                s.expected.in = std::fopen(against, "rb");
                uint8_t header[kHeaderBytes];
                if (!s.expected.in || std::fread(header, 1, kHeaderBytes, s.expected.in) != kHeaderBytes ||
                    std::memcmp(header, kMagic, 8) != 0)
                {
                    std::fprintf(stderr, "[calllog] %s: not a PS2XCLG1 log\n", against);
                    if (s.expected.in)
                        std::fclose(s.expected.in);
                    s.expected.in = nullptr;
                    s.mode = Mode::Off;
                    return false;
                }
                std::fprintf(stderr, "[calllog] %s against %s%s\n", s.mode == Mode::Replay ? "replaying" : "checking",
                             against, s.strict ? " (strict)" : "");
                g_replay = s.mode == Mode::Replay;
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
        const auto &kernel = kernelNames();
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t address = names[i].address & (PS2_RAM_SIZE - 1u);
            s.names.emplace_back(address, names[i].name);
            s.classes[address] = kernel.count(names[i].name) ? 1u : 2u;
        }
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
        if (s.call.open || !s.bound)
            return false;
        if (s.mode == Mode::Replay)
        {
            bool native = false;
            if (replayCall(s, ctx, kind, target, 0, nullptr, native))
                return true;
        }
        return callEnterImpl(s, ctx, runtime, kind, target, token);
    }

    bool ioEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, capture::OutKind kind, uint32_t address,
                 uint32_t size, void *value, uint32_t &token)
    {
        (void)rdram;
        State &s = state();
        if (s.mode == Mode::Off)
            return false;
        bindNow(s, runtime);
        if (s.call.open || !s.bound)
            return false;
        if (s.mode == Mode::Replay)
        {
            bool native = false;
            if (replayCall(s, ctx, kind, address, size, value, native))
                return true;
        }
        (void)callEnterImpl(s, ctx, runtime, kind, address, token);
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
            if (s.scheduler->transferToDispatcherPending())
                c.flags |= kCallTransfer;
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
            collect(s);
            mergeCallPages(s);
            uint32_t runs = 0;
            for (uint32_t p : s.pages)
                runs += diffPage(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), p * kPage, b);
            putTraces(b, c.traceFrom, c.ownTrace);
            b.patch32(c.outAt + 24, static_cast<uint32_t>(words.size()));
            b.patch32(c.outAt + 28, runs);
            const uint64_t after = s.scheduler->currentCycle();
            std::memcpy(b.b.data() + 72, &after, 8);
            const uint32_t spinAfter = static_cast<uint32_t>(s.runtime->memory().pendingTimerSpinCycles());
            std::memcpy(b.b.data() + 84, &spinAfter, 4);
            b.b[1] = c.flags;
            finishRecord(b, 0);
            c.open = false;
            ++s.records;
            ++s.typeCounts[kCall];
            if (s.mode == Mode::Record)
            {
                shadowCatchUp(s);
                s.chunk.put(b.b.data(), b.size());
            }
            else
            {
                const uint8_t *want = compareRecord(s, b.b.data(), b.size());
                if (want && s.mode == Mode::Check)
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
                shadowCatchUp(s);
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
        if (source == kSrcResume && s.scheduler && s.mode == Mode::Replay)
        {
            // A library's wait completion (the only ones there are) changes its thread's context: the log's words.
            if (const GuestThread *t = s.scheduler->currentThread())
                s.logContexts.insert(contextKey(t->id, static_cast<uint32_t>(t->invocations.size())));
        }
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
        // The replay: the operations a library's thread-end observer made, made again (the others run inside their
        // continuation).
        if (s.mode == Mode::Replay && s.source == kSrcThreadEnd)
            replayContinuation(kSrcThreadEnd);
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
            bool expected = false;
            if (s.mode != Mode::Record && !s.desynced)
            {
                const uint8_t *want = s.expected.peek();
                expected = want && want[0] == kCtx && rd32(want + 40) == static_cast<uint32_t>(thread) &&
                           rd32(want + 44) == depth && rd64(want + 8) == scheduler.currentCycle();
            }
            if (diffContext(before, now, mask, words) != 0 || expected)
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
                if (s.mode != Mode::Record)
                {
                    ++s.records;
                    ++s.typeCounts[kCtx];
                    s.expectedDifference = s.logContexts.erase(contextKey(thread, depth)) != 0;
                    if (const uint8_t *want = compareRecord(s, b.b.data(), b.size()))
                    {
                        uint32_t rec[kCtxWords];
                        std::memcpy(rec, before, kCtxBytes);
                        const uint32_t *wantMask = reinterpret_cast<const uint32_t *>(want + 56);
                        applyContextWords(wantMask, wantMask + kMaskWords, rec);
                        fromRecord(rec, context);
                        ++s.substituted;
                    }
                }
                else
                    emit(s, b);
            }
        }
        catch (const EeDispatcherTransfer &)
        {
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
        if (s.mode == Mode::Replay && (s.reissuing || s.replayInvocations.erase(invocation.sequence)))
            s.logContexts.insert(contextKey(thread, depth));
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
        if (s.mode == Mode::Replay && invocation.onComplete)
            s.logContexts.insert(contextKey(thread, depth - 1u)); // a library's handler completion changes the context below
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

    bool op(uint32_t kind, const uint32_t *args, uint32_t count, uint64_t wide) noexcept
    {
        State &s = state();
        if (s.mode == Mode::Off || !s.bound)
            return false;
        try
        {
            Buf b;
            head(s, b, kOpRecord, 0, static_cast<uint16_t>(kind));
            const uint32_t origin = currentOrigin(s);
            b.u32(origin);
            b.u32(count);
            b.u32(s.opDepth);
            b.u32(0);
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
        ++s.opDepth;
        return true;
    }

    void opEnd() noexcept
    {
        State &s = state();
        if (s.opDepth)
            --s.opDepth;
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
        if (op(kOpQueue, args, 12, invocation.tag))
            opEnd();
        if (s.reissuing)
            s.replayInvocations.insert(invocation.sequence);
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
        if (op(kOpBlock, args, 6, wide))
            opEnd();
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
        if (s.mode == Mode::Off || s.mode == Mode::Replay || !s.bound || s.call.open || s.iopOpen)
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
        if (s.mode == Mode::Replay)
        {
            if (s.bound && !s.call.open && !s.desynced)
                replayIop(s, site);
            return;
        }
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
        Buf b;
        head(s, b, kFrame, 0, 0);
        b.u32(static_cast<uint32_t>(ps2x::det::frame()));
        if (s.call.open)
        {
            // Inside a call: what it had written and the calls it had made when the frame ended (the frame's record
            // sees those; what it does after belongs to the next frame).
            OpenCall &c = s.call;
            c.flags |= kCallFrameInside;
            Buf runs;
            uint32_t count = 0;
            if (s.mode == Mode::Replay && s.replayFrame)
            {
                // Made again from the log's (the replay answered the call).
                const uint8_t *f = s.replayFrame;
                b.put(f + 44, rd32(f + 4) - 44);
                finishRecord(b, 0);
                emit(s, b);
                c.traceFrom = 0;
                return;
            }
            if (s.mode != Mode::Replay)
            {
                collect(s);
                mergeCallPages(s);
                for (uint32_t p : s.pages)
                    count += diffPage(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), p * kPage, runs);
                c.earlierPages = s.pages;
            }
            b.u32(count);
            Buf traces;
            if (s.mode != Mode::Replay)
                putTraces(traces, c.traceFrom, c.ownTrace);
            else
                traces.u32(0);
            b.u32(rd32(traces.b.data()));
            b.u32(0);
            b.put(runs.b.data(), runs.size());
            b.put(traces.b.data() + 4, traces.size() - 4);
            c.traceFrom = 0; // the reference starts the next frame's list
        }
        else
        {
            b.u32(0);
            b.u32(0);
            b.u32(0);
        }
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
        if (s.mode != Mode::Record)
        {
            // The run ends inside a call (the reference's frame limit ends it at a frame end inside one): the records
            // made inside it so far are compared.
            if (s.call.open && s.mode == Mode::Check)
            {
                s.call.flags |= kCallCut;
                const int saved = s.call.uncaught;
                s.call.uncaught = std::uncaught_exceptions();
                callLeave(s.call.id);
                (void)saved;
            }
            writeReport(s);
            std::fflush(stderr);
            return;
        }
        if (s.call.open)
        {
            s.call.flags |= kCallCut;
            s.call.uncaught = std::uncaught_exceptions();
            callLeave(s.call.id);
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

    bool replayUnmaskRaises(uint32_t cause) noexcept
    {
        State &s = state();
        if (s.mode != Mode::Replay || s.desynced || !s.call.open)
            return false;
        // The records inside the kernel call being made: a cause-enable for this channel followed by its irq.
        for (size_t off : s.expected.insideCall())
        {
            const uint8_t *p = s.expected.raw.data() + off;
            if (p[0] == kOpRecord && (p[2] | (p[3] << 8)) == kOpIrq && rd32(p + 72) == 1u && rd32(p + 76) == cause)
                return true;
        }
        return false;
    }

    void replayOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &s = state();
        uint32_t token = 0;
        if (s.mode == Mode::Replay && !s.call.open)
        {
            if (callEnter(rdram, ctx, runtime, capture::kOutOverride, ctx->pc, token))
                return;
        }
        // Not answered (made inside a call being made): the port's function itself.
        if (token)
            callLeave(token);
        runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
    }
}
