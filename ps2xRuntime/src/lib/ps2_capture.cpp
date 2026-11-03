// The per-function capture and its self-check (ps2_capture.h). The file format is BozoEngine's docs/specs/capture.md
// ("PS2XCAP1").

#include "runtime/ps2_capture.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_determinism.h"
#include "runtime/ps2_reference.h"

#include <zlib.h>

#include <algorithm>
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

extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;

namespace ps2x::capture
{
    bool g_active = false;

    namespace watch
    {
        uint8_t *allocate(size_t bytes);
        void release(uint8_t *memory);
        size_t collect(uint8_t *base, size_t bytes, uint32_t *pages, size_t maxPages);
    }
}

namespace ps2x::capture
{
    namespace
    {
        // ---- The format's constants (capture.md) --------------------------------------------------------------------
        constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'C', 'A', 'P', '1'};
        constexpr uint32_t kVersion = 1;
        constexpr uint32_t kHeaderBytes = 128;
        constexpr uint32_t kCtxWords = 332;
        constexpr uint32_t kCtxBytes = kCtxWords * 4u;
        constexpr uint32_t kMaskWords = (kCtxWords + 31u) / 32u;
        constexpr uint32_t kRamBytes = PS2_RAM_SIZE;
        constexpr uint32_t kSprBytes = PS2_SCRATCHPAD_SIZE;
        constexpr uint32_t kVu0Bytes = PS2_VU0_DATA_SIZE;
        constexpr uint32_t kFlatSpr = kRamBytes;
        constexpr uint32_t kFlatVu0 = kRamBytes + kSprBytes;
        constexpr uint32_t kFlatBytes = kRamBytes + kSprBytes + kVu0Bytes;
        constexpr uint32_t kPage = 4096u;
        constexpr uint32_t kRamPages = kRamBytes / kPage;
        constexpr uint32_t kPages = kFlatBytes / kPage;
        constexpr uint32_t kSampleHeaderBytes = 64;
        constexpr uint32_t kOutHeaderBytes = 36;

        constexpr uint32_t tag(const char (&s)[5])
        {
            return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
                   uint32_t(uint8_t(s[3])) << 24;
        }
        constexpr uint32_t kTagSample = tag("SAMP");
        constexpr uint32_t kTagSkip = tag("SKIP");
        constexpr uint32_t kTagOverrides = tag("OVRD");
        constexpr uint32_t kTagEnd = tag("END ");

        // Why a call could not be sampled (SKIP chunks' reasons).
        enum Reason : uint32_t
        {
            kThreadSwitch = 1u << 0, // an exception unwound the call (the scheduler switched guest threads) or the
                                     // guest thread changed
            kInterrupted = 1u << 1,  // a scheduler checkpoint fired inside the call (events, interrupts, a time slice)
            kUnwound = 1u << 2,      // the call left through the dispatcher (a jump out of it, no return to ra)
            kException = 1u << 3,    // BREAK, a trap, an exception or a TLB instruction
            kTooBig = 1u << 4,       // more than the size limit of memory written
            kCop0Timer = 1u << 5,    // COP0 Count or a performance counter
            kAbandoned = 1u << 6,    // the run ended inside the call
        };

        enum class Mode
        {
            Off,
            Capture,
            Replay,
        };

        struct Config
        {
            std::string dir;
            uint32_t samples = 4;
            uint32_t from = 0;
            uint32_t to = 18000;
            uint32_t tries = 12;
            uint64_t sizeLimit = 96ull << 20;
            std::unordered_set<uint32_t> funcs;
            int checkLog = 20;
        };

        // ---- Buffers --------------------------------------------------------------------------------------------------
        struct Buf
        {
            std::vector<uint8_t> b;
            void u8(uint8_t v) { b.push_back(v); }
            void u16(uint16_t v) { put(&v, 2); }
            void u32(uint32_t v) { put(&v, 4); }
            void u64(uint64_t v) { put(&v, 8); }
            void put(const void *p, size_t n)
            {
                const uint8_t *s = static_cast<const uint8_t *>(p);
                b.insert(b.end(), s, s + n);
            }
            void pad4()
            {
                while (b.size() & 3u)
                    b.push_back(0);
            }
            size_t size() const { return b.size(); }
            void patch32(size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
        };

        // ---- The context record (capture.md, "Context") ---------------------------------------------------------------
        uint32_t bitsOf(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        float floatOf(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }

        void toRecord(const R5900Context &c, uint32_t *w)
        {
            std::memset(w, 0, kCtxBytes);
            for (int i = 0; i < 32; ++i)
                std::memcpy(w + i * 4, &c.r[i], 16);
            std::memcpy(w + 128, &c.hi, 8);
            std::memcpy(w + 130, &c.lo, 8);
            std::memcpy(w + 132, &c.hi1, 8);
            std::memcpy(w + 134, &c.lo1, 8);
            w[136] = c.pc;
            w[137] = c.sa;
            for (int i = 0; i < 32; ++i)
                w[138 + i] = bitsOf(c.f[i]);
            w[170] = c.fcr31;
            w[171] = bitsOf(c.f_acc);
            for (int i = 0; i < 32; ++i)
                std::memcpy(w + 172 + i * 4, &c.vu0_vf[i], 16);
            std::memcpy(w + 300, &c.vu0_acc, 16);
            for (int i = 0; i < 16; ++i)
                w[304 + i] = c.vi[i];
            w[320] = bitsOf(c.vu0_q);
            w[321] = bitsOf(c.vu0_i);
            w[322] = bitsOf(c.vu0_p);
            std::memcpy(w + 323, &c.vu0_r, 4);
            w[324] = c.vu0_status;
            w[325] = c.vu0_mac_flags;
            w[326] = c.vu0_clip_flags;
            w[327] = c.vu0_cmsar0;
            w[328] = c.cop0_status;
        }

        void fromRecord(const uint32_t *w, R5900Context &c)
        {
            for (int i = 0; i < 32; ++i)
                std::memcpy(&c.r[i], w + i * 4, 16);
            std::memcpy(&c.hi, w + 128, 8);
            std::memcpy(&c.lo, w + 130, 8);
            std::memcpy(&c.hi1, w + 132, 8);
            std::memcpy(&c.lo1, w + 134, 8);
            c.pc = w[136];
            c.sa = w[137];
            for (int i = 0; i < 32; ++i)
                c.f[i] = floatOf(w[138 + i]);
            c.fcr31 = w[170];
            c.f_acc = floatOf(w[171]);
            for (int i = 0; i < 32; ++i)
                std::memcpy(&c.vu0_vf[i], w + 172 + i * 4, 16);
            std::memcpy(&c.vu0_acc, w + 300, 16);
            for (int i = 0; i < 16; ++i)
                c.vi[i] = static_cast<uint16_t>(w[304 + i]);
            c.vu0_q = floatOf(w[320]);
            c.vu0_i = floatOf(w[321]);
            c.vu0_p = floatOf(w[322]);
            std::memcpy(&c.vu0_r, w + 323, 4);
            c.vu0_status = static_cast<uint16_t>(w[324]);
            c.vu0_mac_flags = w[325];
            c.vu0_clip_flags = w[326];
            c.vu0_cmsar0 = w[327];
            c.cop0_status = w[328];
        }

        std::string wordName(uint32_t i)
        {
            static const char *const gpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                                "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                                "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
            char text[48];
            if (i < 128)
                std::snprintf(text, sizeof(text), "%s.w%u", gpr[i / 4], i % 4);
            else if (i < 136)
            {
                static const char *const n[4] = {"hi", "lo", "hi1", "lo1"};
                std::snprintf(text, sizeof(text), "%s.w%u", n[(i - 128) / 2], (i - 128) % 2);
            }
            else if (i == 136)
                return "pc";
            else if (i == 137)
                return "sa";
            else if (i < 170)
                std::snprintf(text, sizeof(text), "f%u", i - 138);
            else if (i == 170)
                return "fcr31";
            else if (i == 171)
                return "facc";
            else if (i < 300)
                std::snprintf(text, sizeof(text), "vf%u.%c", (i - 172) / 4, "xyzw"[(i - 172) % 4]);
            else if (i < 304)
                std::snprintf(text, sizeof(text), "acc.%c", "xyzw"[i - 300]);
            else if (i < 320)
                std::snprintf(text, sizeof(text), "vi%u", i - 304);
            else
            {
                static const char *const n[12] = {"Q", "I", "P", "R", "vu0_status", "vu0_mac", "vu0_clip",
                                                  "vu0_cmsar0", "cop0_status", "reserved", "reserved", "reserved"};
                return n[i - 320];
            }
            return text;
        }

        // Runs of the bytes that differ between two pages (a run ends at 8 equal bytes), each as {address, length,
        // bytes, padding to 4} at flat address base + offset.
        uint32_t diffPage(const uint8_t *before, const uint8_t *after, uint32_t base, Buf &out)
        {
            if (std::memcmp(before, after, kPage) == 0)
                return 0;
            uint32_t runs = 0;
            uint32_t i = 0;
            while (i < kPage)
            {
                if (before[i] == after[i])
                {
                    ++i;
                    continue;
                }
                const uint32_t start = i;
                uint32_t end = i + 1;
                uint32_t same = 0;
                for (uint32_t j = i + 1; j < kPage; ++j)
                {
                    if (before[j] == after[j])
                    {
                        if (++same >= 8u)
                            break;
                    }
                    else
                    {
                        same = 0;
                        end = j + 1;
                    }
                }
                out.u32(base + start);
                out.u32(end - start);
                out.put(after + start, end - start);
                out.pad4();
                ++runs;
                i = end;
            }
            return runs;
        }

        // ---- State ----------------------------------------------------------------------------------------------------
        struct OpenSample
        {
            bool active = false;
            bool suspended = false; // its code jumped out: the dispatcher runs the rest (EeScheduler::run)
            uint32_t id = 0;
            uint32_t function = 0;
            uint32_t slot = 0;
            uint32_t frame = 0;
            int thread = 0;
            int uncaught = 0;
            uint64_t callIndex = 0;
            uint32_t ra = 0;
            uint32_t sp = 0;
            uint32_t reasons = 0;
            R5900Context *ctx = nullptr;
            uint32_t entryWords[kCtxWords];
            Buf entryRuns;
            uint32_t entryRunCount = 0;
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> undo; // chain pages before this entry
            Buf outcalls;
            uint32_t outcallCount = 0;
            // The out-call in progress (one at a time; calls it makes are part of it).
            bool inOutcall = false;
            int outUncaught = 0;
            size_t outOffset = 0;
            uint32_t outWords[kCtxWords];
            R5900Context *outCtx = nullptr;
        };

        struct FnStats
        {
            uint32_t samples = 0;
            uint32_t fails = 0;
            uint32_t reasons[8] = {};
        };

        struct Replay
        {
            bool active = false;
            const uint8_t *cur = nullptr;
            const uint8_t *end = nullptr;
            uint32_t index = 0;
            uint64_t checkpoints = 0;
            bool runaway = false;
        };

        struct Divergence
        {
            std::string what;
        };

        struct State
        {
            Mode mode = Mode::Off;
            bool configured = false;
            Config cfg;
            uint8_t *watched = nullptr;
            PS2Runtime *runtime = nullptr;
            uint8_t *ram = nullptr;
            uint8_t *spr = nullptr;
            uint8_t *vu0 = nullptr;
            std::vector<uint8_t> chain;  // L: the memory at the last sample's entry (what the format's chain holds)
            std::vector<uint8_t> shadow; // S: the memory at the last boundary (out-call diffs)
            std::vector<uint8_t> dirty;  // pages that may differ from the chain
            std::vector<uint32_t> pages; // scratch: the pages of one collect
            bool watchBroken = false;
            FILE *out = nullptr;
            std::string path;
            uint64_t bytes = 0;
            uint32_t samples = 0;
            uint32_t skips = 0;
            uint32_t nextId = 1;
            std::vector<uint32_t> next;   // per function table slot: the first frame a sample may be tried
            std::vector<uint64_t> calls;  // per slot: calls seen
            std::vector<uint8_t> overrideSlots;
            std::unordered_map<uint32_t, void *> originals;
            std::vector<uint32_t> overrideList;
            bool overridesWritten = false;
            std::map<uint32_t, FnStats> stats;
            OpenSample open;
            Replay replay;
        };

        State &state()
        {
            static State s;
            return s;
        }

        uint8_t *flatPage(State &s, uint32_t page)
        {
            if (page < kRamPages)
                return s.ram + page * kPage;
            if (page < kRamPages + kSprBytes / kPage)
                return s.spr + (page - kRamPages) * kPage;
            return s.vu0 + (page - kRamPages - kSprBytes / kPage) * kPage;
        }

        uint8_t *flatByte(State &s, uint32_t address)
        {
            if (address < kFlatSpr)
                return s.ram + address;
            if (address < kFlatVu0)
                return s.spr + (address - kFlatSpr);
            return s.vu0 + (address - kFlatVu0);
        }

        bool slotOf(uint32_t address, uint32_t &slot)
        {
            if ((address & 3u) != 0u || address < g_ps2RecompiledFunctionTableBase ||
                address >= g_ps2RecompiledFunctionTableEnd)
                return false;
            slot = (address - g_ps2RecompiledFunctionTableBase) >> 2;
            return slot < g_ps2RecompiledFunctionTableSlotCount;
        }

        uint32_t parseNumber(const char *text)
        {
            return static_cast<uint32_t>(std::strtoul(text, nullptr, 0));
        }

        void bindMemory(State &s, PS2Runtime *runtime)
        {
            if (s.runtime)
                return;
            s.runtime = runtime;
            s.ram = runtime->memory().getRDRAM();
            s.spr = runtime->memory().getScratchpad();
            s.vu0 = runtime->memory().getVU0Data();
            if (s.ram != s.watched)
            {
                std::fprintf(stderr, "[capture] main RAM is not write-tracked: every page is compared at each boundary\n");
                s.watchBroken = true;
            }
        }

        // The pages written since the last call (main RAM from the write tracking, the scratchpad and VU0's data
        // memory always), each marked as possibly differing from the chain.
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
            for (uint32_t p : s.pages)
                s.dirty[p] = 1;
        }

        void syncShadow(State &s)
        {
            collect(s);
            for (uint32_t p : s.pages)
                std::memcpy(s.shadow.data() + size_t(p) * kPage, flatPage(s, p), kPage);
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

        void writeOverrides(State &s)
        {
            if (!s.out)
                return;
            Buf b;
            b.u32(static_cast<uint32_t>(s.overrideList.size()));
            for (uint32_t a : s.overrideList)
                b.u32(a);
            writeChunk(s, kTagOverrides, b, false);
            s.overridesWritten = true;
        }

        bool openFile(State &s)
        {
            std::error_code ec;
            std::filesystem::create_directories(s.cfg.dir, ec);
            s.path = (std::filesystem::path(s.cfg.dir) / "capture.bin").string();
            s.out = std::fopen(s.path.c_str(), "wb");
            if (!s.out)
            {
                std::fprintf(stderr, "[capture] cannot write %s: capture off\n", s.path.c_str());
                return false;
            }
            uint8_t header[kHeaderBytes] = {};
            std::memcpy(header, kMagic, 8);
            const uint32_t fields[] = {kVersion, kHeaderBytes, kCtxBytes, kFlatBytes, kRamBytes, kSprBytes, kVu0Bytes,
                                       s.cfg.samples, s.cfg.from, s.cfg.to};
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
            const uint32_t pageBytes = kPage;
            std::memcpy(header + 56, &pageBytes, 4);
            std::string run = std::filesystem::path(s.cfg.dir).filename().string();
            if (const char *name = std::getenv("PS2X_REFERENCE_RUN"); name && *name)
                run = name;
            std::strncpy(reinterpret_cast<char *>(header + 64), run.c_str(), 63);
            std::fwrite(header, 1, sizeof(header), s.out);
            s.bytes = sizeof(header);
            std::fprintf(stderr, "[capture] %s: %u samples a function over frames %u-%u\n", s.path.c_str(),
                         s.cfg.samples, s.cfg.from, s.cfg.to);
            return true;
        }

        void writeSummary(State &s)
        {
            const std::string path = (std::filesystem::path(s.cfg.dir) / "summary.txt").string();
            FILE *f = std::fopen(path.c_str(), "w");
            if (!f)
                return;
            std::fprintf(f, "# function samples fails calls threadSwitch interrupted unwound exception tooBig cop0 abandoned\n");
            // Every guest function called during the run, sampled or not (one called only inside sampled calls, or
            // only outside the frame window, is never tried).
            for (uint32_t slot = 0; slot < s.calls.size(); ++slot)
            {
                if (s.calls[slot] == 0)
                    continue;
                const uint32_t address = g_ps2RecompiledFunctionTableBase + slot * 4u;
                const auto it = s.stats.find(address);
                const FnStats st = it != s.stats.end() ? it->second : FnStats{};
                std::fprintf(f, "0x%06x %u %u %" PRIu64, address, st.samples, st.fails, s.calls[slot]);
                for (int i = 0; i < 7; ++i)
                    std::fprintf(f, " %u", st.reasons[i]);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }

        // ---- Sampling -------------------------------------------------------------------------------------------------
        uint32_t slotFrame(const Config &c, uint32_t index)
        {
            return c.from + static_cast<uint32_t>(uint64_t(c.to - c.from) * index / std::max(1u, c.samples));
        }

        void closeSample(State &s, uint32_t reasons)
        {
            OpenSample &o = s.open;
            FnStats &st = s.stats[o.function];
            const uint32_t frame = static_cast<uint32_t>(ps2x::det::frame());
            if (reasons == 0)
            {
                syncShadow(s);
                R5900Context *ctx = o.ctx;
                uint32_t exitWords[kCtxWords];
                toRecord(*ctx, exitWords);
                Buf exitRuns;
                uint32_t exitRunCount = 0;
                for (uint32_t p = 0; p < kPages; ++p)
                    if (s.dirty[p])
                        exitRunCount += diffPage(s.chain.data() + size_t(p) * kPage, flatPage(s, p), p * kPage, exitRuns);
                if (o.entryRuns.size() + o.outcalls.size() + exitRuns.size() > s.cfg.sizeLimit)
                    reasons |= kTooBig;
                else
                {
                    Buf b;
                    b.b.reserve(kSampleHeaderBytes + 2 * kCtxBytes + o.entryRuns.size() + o.outcalls.size() +
                                exitRuns.size());
                    b.u32(o.function);
                    b.u32(s.samples);
                    b.u32(o.frame);
                    b.u32(static_cast<uint32_t>(o.thread));
                    b.u64(o.callIndex);
                    b.u32(st.samples); // which of the function's samples
                    b.u32(o.outcallCount);
                    b.u32(o.entryRunCount);
                    b.u32(static_cast<uint32_t>(o.entryRuns.size()));
                    b.u32(static_cast<uint32_t>(o.outcalls.size()));
                    b.u32(exitRunCount);
                    b.u32(static_cast<uint32_t>(exitRuns.size()));
                    b.u32(o.ra);
                    b.u32(o.sp);
                    b.u32(0);
                    b.put(o.entryWords, kCtxBytes);
                    b.put(o.entryRuns.b.data(), o.entryRuns.size());
                    b.put(o.outcalls.b.data(), o.outcalls.size());
                    b.put(exitWords, kCtxBytes);
                    b.put(exitRuns.b.data(), exitRuns.size());
                    writeChunk(s, kTagSample, b, true);
                    ++s.samples;
                    ++st.samples;
                    s.next[o.slot] = st.samples >= s.cfg.samples
                                         ? UINT32_MAX
                                         : std::max(slotFrame(s.cfg, st.samples), frame + 1u);
                }
            }
            if (reasons != 0)
            {
                // The chain goes back to the last written sample's entry.
                for (auto &[page, bytes] : o.undo)
                {
                    std::memcpy(s.chain.data() + size_t(page) * kPage, bytes.data(), kPage);
                    s.dirty[page] = 1;
                }
                Buf b;
                b.u32(o.function);
                b.u32(o.frame);
                b.u32(reasons);
                b.u32(static_cast<uint32_t>(o.thread));
                b.u64(o.callIndex);
                b.u32(o.outcallCount);
                b.u32(0);
                writeChunk(s, kTagSkip, b, false);
                ++s.skips;
                ++st.fails;
                for (int i = 0; i < 7; ++i)
                    if (reasons & (1u << i))
                        ++st.reasons[i];
                s.next[o.slot] = st.fails >= s.cfg.tries ? UINT32_MAX : frame + 1u;
            }
            o.active = false;
            o.suspended = false;
            o.ctx = nullptr;
            o.undo.clear();
            o.entryRuns.b.clear();
            o.outcalls.b.clear();
            o.inOutcall = false;
        }

        uint32_t openSample(State &s, R5900Context *ctx, uint32_t start, uint32_t slot)
        {
            if (!s.out && !openFile(s))
            {
                s.mode = Mode::Off;
                g_active = false;
                return 0;
            }
            if (!s.overridesWritten)
                writeOverrides(s);
            OpenSample &o = s.open;
            o.active = true;
            o.id = s.nextId++;
            if (o.id == 0)
                o.id = s.nextId++;
            o.function = start;
            o.slot = slot;
            o.frame = static_cast<uint32_t>(ps2x::det::frame());
            o.thread = s.runtime->eeScheduler().currentThreadId();
            o.uncaught = std::uncaught_exceptions();
            o.callIndex = s.calls[slot];
            o.ra = getRegU32(ctx, 31);
            o.sp = getRegU32(ctx, 29);
            o.reasons = 0;
            o.suspended = false;
            o.ctx = ctx;
            o.outcallCount = 0;
            o.inOutcall = false;
            toRecord(*ctx, o.entryWords);

            // The memory at entry: the pages that differ from the chain, written as its next step.
            syncShadow(s);
            o.entryRunCount = 0;
            for (uint32_t p = 0; p < kPages; ++p)
            {
                if (!s.dirty[p])
                    continue;
                s.dirty[p] = 0;
                uint8_t *chainPage = s.chain.data() + size_t(p) * kPage;
                const uint8_t *now = flatPage(s, p);
                if (std::memcmp(chainPage, now, kPage) == 0)
                    continue;
                o.undo.emplace_back(p, std::vector<uint8_t>(chainPage, chainPage + kPage));
                o.entryRunCount += diffPage(chainPage, now, p * kPage, o.entryRuns);
                std::memcpy(chainPage, now, kPage);
            }
            return o.id;
        }

        // ---- Replay ---------------------------------------------------------------------------------------------------
        struct OutRecord
        {
            uint8_t kind;
            uint8_t size;
            uint16_t flags;
            uint32_t target;
            uint32_t value[4];
            uint32_t ctxCount;
            uint32_t runCount;
            const uint32_t *mask;
            const uint32_t *words;
            const uint8_t *runs;
            const uint8_t *next;
        };

        uint32_t rd32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        const uint8_t *skipRuns(const uint8_t *p, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t len = rd32(p + 4);
                p += 8 + ((len + 3u) & ~3u);
            }
            return p;
        }

        OutRecord parseOut(const uint8_t *p)
        {
            OutRecord r{};
            r.kind = p[0];
            r.size = p[1];
            std::memcpy(&r.flags, p + 2, 2);
            r.target = rd32(p + 4);
            std::memcpy(r.value, p + 8, 16);
            r.ctxCount = rd32(p + 24);
            r.runCount = rd32(p + 28);
            r.mask = reinterpret_cast<const uint32_t *>(p + kOutHeaderBytes);
            r.words = r.mask + kMaskWords;
            r.runs = reinterpret_cast<const uint8_t *>(r.words + r.ctxCount);
            r.next = skipRuns(r.runs, r.runCount);
            return r;
        }

        void applyRuns(State &s, const uint8_t *p, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t address = rd32(p);
                const uint32_t len = rd32(p + 4);
                if (address + len <= kFlatBytes)
                {
                    // A run may cross the regions: byte by byte through the flat map where it does.
                    if ((address < kFlatSpr) == ((address + len - 1) < kFlatSpr) &&
                        (address < kFlatVu0) == ((address + len - 1) < kFlatVu0))
                        std::memcpy(flatByte(s, address), p + 8, len);
                    else
                        for (uint32_t j = 0; j < len; ++j)
                            *flatByte(s, address + j) = p[8 + j];
                }
                p += 8 + ((len + 3u) & ~3u);
            }
        }

        void applyContext(const uint32_t *mask, const uint32_t *words, R5900Context *ctx)
        {
            uint32_t w[kCtxWords];
            toRecord(*ctx, w);
            uint32_t k = 0;
            for (uint32_t i = 0; i < kCtxWords; ++i)
                if (mask[i / 32] & (1u << (i % 32)))
                    w[i] = words[k++];
            fromRecord(w, *ctx);
        }

        const char *kindName(uint32_t kind)
        {
            static const char *const names[] = {"?", "stub", "override", "syscall", "vu0", "io-load", "io-store",
                                                "exception"};
            return kind < 8 ? names[kind] : "?";
        }

        // The next out-call of the replayed sample: it must be this one; its effects are applied.
        OutRecord replayOut(State &s, R5900Context *ctx, OutKind kind, uint32_t target)
        {
            Replay &r = s.replay;
            char text[200];
            if (r.cur >= r.end)
            {
                std::snprintf(text, sizeof(text), "out-call %u: %s 0x%x, none left in the capture", r.index,
                              kindName(kind), target);
                throw Divergence{text};
            }
            OutRecord rec = parseOut(r.cur);
            if (rec.kind != kind || rec.target != target)
            {
                std::snprintf(text, sizeof(text), "out-call %u: %s 0x%x, the capture has %s 0x%x", r.index,
                              kindName(kind), target, kindName(rec.kind), rec.target);
                throw Divergence{text};
            }
            if (kind == kOutStub || kind == kOutOverride)
            {
                for (int i = 0; i < 4; ++i)
                    if (getRegU32(ctx, 4 + i) != rec.value[i])
                    {
                        std::snprintf(text, sizeof(text), "out-call %u: %s 0x%x, a%d 0x%x, the capture has 0x%x",
                                      r.index, kindName(kind), target, i, getRegU32(ctx, 4 + i), rec.value[i]);
                        throw Divergence{text};
                    }
            }
            applyContext(rec.mask, rec.words, ctx);
            applyRuns(s, rec.runs, rec.runCount);
            r.cur = rec.next;
            ++r.index;
            return rec;
        }

        void readConfig(State &s, const char *dir)
        {
            Config &c = s.cfg;
            c.dir = dir;
            if (const char *v = std::getenv("PS2X_CAPTURE_SAMPLES"); v && *v)
                c.samples = std::max(1u, parseNumber(v));
            if (const char *v = std::getenv("PS2X_REFERENCE_FRAMES"); v && *v)
                c.to = std::max(1u, parseNumber(v));
            if (const char *v = std::getenv("PS2X_CAPTURE_FRAMES"); v && *v)
            {
                c.from = parseNumber(v);
                if (const char *dash = std::strchr(v, '-'))
                    c.to = parseNumber(dash + 1);
            }
            if (c.to <= c.from)
                c.to = c.from + 1;
            if (const char *v = std::getenv("PS2X_CAPTURE_TRIES"); v && *v)
                c.tries = std::max(1u, parseNumber(v));
            if (const char *v = std::getenv("PS2X_CAPTURE_FUNCS"); v && *v)
            {
                const char *p = v;
                while (*p)
                {
                    char *endp = nullptr;
                    const unsigned long a = std::strtoul(p, &endp, 0);
                    if (endp == p)
                        break;
                    c.funcs.insert(static_cast<uint32_t>(a));
                    p = endp;
                    while (*p == ',' || *p == ' ')
                        ++p;
                }
            }
            if (const char *v = std::getenv("PS2X_CAPTURE_CHECK_LOG"); v && *v)
                c.checkLog = std::atoi(v);
        }
    }

    // ---- Public ---------------------------------------------------------------------------------------------------------
    bool configure() noexcept
    {
        State &s = state();
        if (s.configured)
            return s.mode != Mode::Off;
        s.configured = true;
        const char *check = std::getenv("PS2X_CAPTURE_CHECK");
        const char *capture = std::getenv("PS2X_CAPTURE");
        if (check && *check)
        {
            readConfig(s, check);
            s.mode = Mode::Replay;
        }
        else if (capture && *capture)
        {
            if (!ps2x::det::enabled())
            {
                std::fprintf(stderr, "[capture] PS2X_CAPTURE needs PS2X_DETERMINISTIC=1: capture off\n");
                return false;
            }
            readConfig(s, capture);
            s.mode = Mode::Capture;
            // The file completed at a normal exit too (PS2X_REFERENCE_FRAMES ends the run through finish()).
            std::atexit([] { finish(); });
        }
        if (s.mode == Mode::Off)
            return false;
        s.chain.assign(kFlatBytes, 0);
        s.shadow.assign(kFlatBytes, 0);
        s.dirty.assign(kPages, 1);
        s.next.assign(g_ps2RecompiledFunctionTableSlotCount, s.cfg.funcs.empty() ? s.cfg.from : UINT32_MAX);
        for (uint32_t a : s.cfg.funcs)
        {
            uint32_t slot = 0;
            if (slotOf(a, slot))
                s.next[slot] = s.cfg.from;
        }
        s.calls.assign(g_ps2RecompiledFunctionTableSlotCount, 0);
        s.overrideSlots.assign(g_ps2RecompiledFunctionTableSlotCount, 0);
        g_active = true;
        return true;
    }

    bool replaying() noexcept { return state().mode == Mode::Replay; }

    uint8_t *allocateRam(size_t bytes)
    {
        State &s = state();
        if (configure() && bytes == kRamBytes && !s.watched)
        {
            s.watched = watch::allocate(bytes);
            if (s.watched)
                return s.watched;
        }
        uint8_t *ram = new uint8_t[bytes];
        return ram;
    }

    void freeRam(uint8_t *ram) noexcept
    {
        State &s = state();
        if (!ram)
            return;
        if (ram == s.watched)
        {
            watch::release(ram);
            s.watched = nullptr;
            return;
        }
        delete[] ram;
    }

    bool plainMemory(uint32_t vaddr) noexcept
    {
        if (ps2IsScratchpadAddress(vaddr))
            return true;
        const uint32_t phys = vaddr >= 0x80000000u && vaddr < 0xC0000000u ? (vaddr & 0x1FFFFFFFu) : vaddr;
        if (phys >= PS2_VU0_DATA_BASE && phys < PS2_VU0_DATA_BASE + PS2_VU0_DATA_SIZE)
            return true;
        return phys < PS2_RAM_SIZE;
    }

    void noteUnreplayable() noexcept
    {
        State &s = state();
        if (s.mode == Mode::Capture && s.open.active && !s.open.inOutcall)
            s.open.reasons |= kCop0Timer;
    }

    void noteFunctionReplaced(uint32_t address, void *original, void *replacement) noexcept
    {
        State &s = state();
        if (!configure())
            return;
        if (!s.originals.count(address))
            s.originals[address] = original;
        uint32_t slot = 0;
        if (!slotOf(address, slot))
            return;
        const bool isOverride = replacement != nullptr && replacement != s.originals[address];
        if (s.overrideSlots[slot] != (isOverride ? 1 : 0))
        {
            s.overrideSlots[slot] = isOverride ? 1 : 0;
            s.overrideList.erase(std::remove(s.overrideList.begin(), s.overrideList.end(), address), s.overrideList.end());
            if (isOverride)
                s.overrideList.push_back(address);
            if (s.overridesWritten)
                writeOverrides(s);
        }
    }

    bool isOverride(uint32_t address) noexcept
    {
        State &s = state();
        uint32_t slot = 0;
        return slotOf(address, slot) && s.overrideSlots[slot] != 0;
    }

    void schedulerPass(bool workPending) noexcept
    {
        State &s = state();
        if (workPending && s.mode == Mode::Capture && s.open.active && s.open.suspended)
            s.open.reasons |= kInterrupted;
    }

    void schedulerResume(R5900Context *context, int thread) noexcept
    {
        State &s = state();
        OpenSample &o = s.open;
        if (s.mode != Mode::Capture || !o.active || !o.suspended)
            return;
        uint32_t reasons = o.reasons;
        if (thread != o.thread || context != o.ctx)
            reasons |= kThreadSwitch;
        else if (reasons == 0 && !(context->pc == o.ra && getRegU32(context, 29) == o.sp))
            return; // still inside the call
        try
        {
            closeSample(s, reasons);
        }
        catch (...)
        {
            o.active = false;
        }
    }

    void noteDispatcherOddity() noexcept
    {
        State &s = state();
        if (s.mode == Mode::Capture && s.open.active && s.open.suspended)
            s.open.reasons |= kUnwound;
    }

    bool checkpoint(bool due) noexcept
    {
        State &s = state();
        if (due && s.open.active && !s.open.inOutcall)
            s.open.reasons |= kInterrupted;
        return due;
    }

    bool replayCheckpoint(PS2Runtime *runtime) noexcept
    {
        State &s = state();
        if (!s.replay.active)
            return false;
        if (++s.replay.checkpoints > 400000000ull)
        {
            s.replay.runaway = true;
            runtime->eeScheduler().noteTransferToDispatcher();
            return true;
        }
        return false;
    }

    uint32_t guestEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t start) noexcept
    {
        (void)rdram;
        State &s = state();
        if (s.mode != Mode::Capture || ctx->pc != start)
            return 0;
        uint32_t slot = 0;
        if (!slotOf(start, slot))
            return 0;
        ++s.calls[slot];
        if (s.open.active)
            return 0;
        const uint64_t frame = ps2x::det::frame();
        if (frame < s.next[slot] || frame >= s.cfg.to)
            return 0;
        bindMemory(s, runtime);
        try
        {
            return openSample(s, ctx, start, slot);
        }
        catch (...)
        {
            s.open.active = false;
            return 0;
        }
    }

    void guestLeave(uint32_t token) noexcept
    {
        State &s = state();
        OpenSample &o = s.open;
        if (!o.active || o.id != token)
            return;
        uint32_t reasons = o.reasons;
        R5900Context *ctx = o.ctx;
        if (std::uncaught_exceptions() > o.uncaught)
            reasons |= kThreadSwitch;
        else
        {
            if (s.runtime->eeScheduler().currentThreadId() != o.thread)
                reasons |= kThreadSwitch;
            if (s.runtime->isStopRequested())
                reasons |= kUnwound;
            const bool returned = ctx->pc == o.ra && getRegU32(ctx, 29) == o.sp &&
                                  !s.runtime->eeScheduler().transferToDispatcherPending();
            if (!returned && reasons == 0)
            {
                // A jump out through the dispatcher (or a fall-through into the next function): the call goes on
                // in EeScheduler::run, which ends it when the context reaches the return address.
                o.suspended = true;
                return;
            }
        }
        try
        {
            closeSample(s, reasons);
        }
        catch (...)
        {
            o.active = false;
        }
    }

    bool nativeEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, OutKind kind, uint32_t target,
                     uint32_t &token)
    {
        (void)rdram;
        (void)runtime;
        State &s = state();
        if (s.mode == Mode::Replay)
        {
            if (!s.replay.active)
                return false;
            replayOut(s, ctx, kind, target);
            return true;
        }
        OpenSample &o = s.open;
        if (s.mode != Mode::Capture || !o.active || o.inOutcall)
            return false;
        if (kind == kOutException)
            o.reasons |= kException;
        syncShadow(s);
        o.inOutcall = true;
        o.outUncaught = std::uncaught_exceptions();
        o.outOffset = o.outcalls.size();
        o.outCtx = ctx;
        toRecord(*ctx, o.outWords);
        Buf &b = o.outcalls;
        b.u8(static_cast<uint8_t>(kind));
        b.u8(0);
        b.u16(0);
        b.u32(target);
        uint32_t value[4] = {};
        if (kind == kOutStub || kind == kOutOverride)
            for (int i = 0; i < 4; ++i)
                value[i] = getRegU32(ctx, 4 + i);
        else if (kind == kOutSyscall)
        {
            value[0] = getRegU32(ctx, 3);
            for (int i = 0; i < 3; ++i)
                value[1 + i] = getRegU32(ctx, 4 + i);
        }
        b.put(value, 16);
        b.u32(0); // context words changed
        b.u32(0); // memory runs
        b.u32(0); // reserved
        token = o.id;
        return false;
    }

    bool ioEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, OutKind kind, uint32_t address,
                 uint32_t size, void *value, uint32_t &token)
    {
        State &s = state();
        if (s.mode == Mode::Replay)
        {
            if (!s.replay.active)
                return false;
            const uint8_t *before = s.replay.cur;
            if (before < s.replay.end)
            {
                const OutRecord peek = parseOut(before);
                char text[200];
                if (peek.kind == kind && peek.target == address &&
                    (peek.size != size || (kind == kOutIoStore && std::memcmp(peek.value, value, size) != 0)))
                {
                    std::snprintf(text, sizeof(text), "out-call %u: %s 0x%x size %u value 0x%x, the capture has size %u value 0x%x",
                                  s.replay.index, kindName(kind), address, size, rd32(static_cast<uint8_t *>(value)),
                                  peek.size, peek.value[0]);
                    throw Divergence{text};
                }
            }
            const OutRecord rec = replayOut(s, ctx, kind, address);
            if (kind == kOutIoLoad)
                std::memcpy(value, rec.value, size);
            return true;
        }
        if (nativeEnter(rdram, ctx, runtime, kind, address, token))
            return true;
        if (token)
        {
            OpenSample &o = s.open;
            o.outcalls.b[o.outOffset + 1] = static_cast<uint8_t>(size);
            if (kind == kOutIoStore)
                std::memcpy(o.outcalls.b.data() + o.outOffset + 8, value, std::min<uint32_t>(size, 16u));
        }
        return false;
    }

    void ioValue(uint32_t token, const void *value, uint32_t size) noexcept
    {
        State &s = state();
        OpenSample &o = s.open;
        if (!token || !o.active || o.id != token || !o.inOutcall)
            return;
        std::memcpy(o.outcalls.b.data() + o.outOffset + 8, value, std::min<uint32_t>(size, 16u));
    }

    void nativeLeave(uint32_t token) noexcept
    {
        State &s = state();
        OpenSample &o = s.open;
        if (!o.active || o.id != token || !o.inOutcall)
            return;
        o.inOutcall = false;
        if (std::uncaught_exceptions() > o.outUncaught)
        {
            o.reasons |= kThreadSwitch;
            o.outcalls.b.resize(o.outOffset);
            return;
        }
        try
        {
            // What the call changed: the context words, the memory (against the shadow, the memory at its entry).
            uint32_t now[kCtxWords];
            toRecord(*o.outCtx, now);
            uint32_t mask[kMaskWords] = {};
            std::vector<uint32_t> words;
            for (uint32_t i = 0; i < kCtxWords; ++i)
                if (now[i] != o.outWords[i])
                {
                    mask[i / 32] |= 1u << (i % 32);
                    words.push_back(now[i]);
                }
            Buf &b = o.outcalls;
            b.put(mask, sizeof(mask));
            b.put(words.data(), words.size() * 4);
            collect(s);
            uint32_t runs = 0;
            for (uint32_t p : s.pages)
            {
                uint8_t *shadowPage = s.shadow.data() + size_t(p) * kPage;
                const uint8_t *page = flatPage(s, p);
                runs += diffPage(shadowPage, page, p * kPage, b);
                std::memcpy(shadowPage, page, kPage);
            }
            b.patch32(o.outOffset + 24, static_cast<uint32_t>(words.size()));
            b.patch32(o.outOffset + 28, runs);
            ++o.outcallCount;
            if (o.entryRuns.size() + b.size() > s.cfg.sizeLimit)
            {
                o.reasons |= kTooBig;
                b.b.clear();
            }
        }
        catch (...)
        {
            o.reasons |= kTooBig;
            o.outcalls.b.clear();
        }
    }

    void frameEnd() noexcept
    {
        State &s = state();
        if (s.mode != Mode::Capture || !s.out)
            return;
        const uint64_t frame = ps2x::det::frame();
        if (frame % 300u == 0u)
        {
            std::fflush(s.out);
            std::fprintf(stderr, "[capture] frame %" PRIu64 ": %u samples, %u skipped, %u functions, %.1f MB\n", frame,
                         s.samples, s.skips, static_cast<uint32_t>(s.stats.size()), s.bytes / 1048576.0);
        }
    }

    void finish() noexcept
    {
        State &s = state();
        if (s.mode != Mode::Capture || !s.out)
            return;
        if (s.open.active)
        {
            Buf b;
            b.u32(s.open.function);
            b.u32(s.open.frame);
            b.u32(kAbandoned);
            b.u32(static_cast<uint32_t>(s.open.thread));
            b.u64(s.open.callIndex);
            b.u32(s.open.outcallCount);
            b.u32(0);
            writeChunk(s, kTagSkip, b, false);
            ++s.skips;
            ++s.stats[s.open.function].reasons[6];
            s.open.active = false;
        }
        uint32_t covered = 0;
        for (const auto &[address, st] : s.stats)
            covered += st.samples ? 1u : 0u;
        Buf b;
        b.u32(s.samples);
        b.u32(s.skips);
        b.u32(covered);
        b.u32(static_cast<uint32_t>(ps2x::det::frame()));
        uint32_t called = 0;
        for (uint64_t calls : s.calls)
            called += calls ? 1u : 0u;
        b.u32(called);
        writeChunk(s, kTagEnd, b, false);
        std::fclose(s.out);
        s.out = nullptr;
        writeSummary(s);
        std::fprintf(stderr,
                     "[capture] done: %u samples of %u functions (%u called), %u calls not replayable, %.1f MB in %s\n",
                     s.samples, covered, called, s.skips, s.bytes / 1048576.0, s.path.c_str());
        std::fflush(stderr);
    }

    bool selfCheckRequested() noexcept { return configure() && state().mode == Mode::Replay; }

    // ---- The self-check -------------------------------------------------------------------------------------------------
    int runSelfCheck(PS2Runtime &runtime)
    {
        State &s = state();
        bindMemory(s, &runtime);
        const std::string path = (std::filesystem::path(s.cfg.dir) / "capture.bin").string();
        FILE *in = std::fopen(path.c_str(), "rb");
        if (!in)
        {
            std::fprintf(stderr, "[capture-check] cannot read %s\n", path.c_str());
            return 2;
        }
        uint8_t header[kHeaderBytes];
        if (std::fread(header, 1, kHeaderBytes, in) != kHeaderBytes || std::memcmp(header, kMagic, 8) != 0 ||
            rd32(header + 16) != kCtxBytes || rd32(header + 20) != kFlatBytes)
        {
            std::fprintf(stderr, "[capture-check] %s: not a PS2XCAP1 capture of this layout\n", path.c_str());
            std::fclose(in);
            return 2;
        }
        const std::string reportPath = (std::filesystem::path(s.cfg.dir) / "selfcheck.txt").string();
        FILE *report = std::fopen(reportPath.c_str(), "w");

        // Memory starts as zeros, like the chain.
        std::memset(s.ram, 0, kRamBytes);
        std::memset(s.spr, 0, kSprBytes);
        std::memset(s.vu0, 0, kVu0Bytes);
        if (!s.watchBroken)
        {
            s.pages.resize(kPages);
            watch::collect(s.ram, kRamBytes, s.pages.data(), kRamPages);
        }

        uint64_t total = 0, matched = 0, diverged = 0, ctxBad = 0, memBad = 0, unconsumed = 0, runaway = 0,
                 unwound = 0, skips = 0;
        std::map<uint32_t, uint32_t> badFunctions;
        int logged = 0;
        std::vector<uint8_t> raw;
        std::vector<uint8_t> packed;
        for (;;)
        {
            uint32_t chunk[4];
            if (std::fread(chunk, sizeof(chunk), 1, in) != 1)
                break;
            packed.resize(chunk[2]);
            if (std::fread(packed.data(), 1, chunk[2], in) != chunk[2])
                break;
            if (chunk[1] & 1u)
            {
                raw.resize(chunk[3]);
                uLongf rawBytes = chunk[3];
                if (uncompress(raw.data(), &rawBytes, packed.data(), chunk[2]) != Z_OK || rawBytes != chunk[3])
                {
                    std::fprintf(stderr, "[capture-check] a damaged chunk: stopped\n");
                    break;
                }
            }
            else
                raw = packed;
            if (chunk[0] == kTagSkip)
                ++skips;
            if (chunk[0] == kTagOverrides)
            {
                const uint32_t n = rd32(raw.data());
                for (uint32_t i = 0; i < n; ++i)
                {
                    uint32_t slot = 0;
                    if (slotOf(rd32(raw.data() + 4 + 4 * i), slot))
                        s.overrideSlots[slot] = 1;
                }
            }
            if (chunk[0] != kTagSample)
                continue;

            const uint8_t *p = raw.data();
            const uint32_t function = rd32(p);
            const uint32_t index = rd32(p + 4);
            const uint32_t outcallCount = rd32(p + 28);
            const uint32_t entryRunCount = rd32(p + 32);
            const uint32_t entryRunBytes = rd32(p + 36);
            const uint32_t outcallBytes = rd32(p + 40);
            const uint32_t exitRunCount = rd32(p + 44);
            const uint32_t returnAddress = rd32(p + 52);
            const uint8_t *entryCtx = p + kSampleHeaderBytes;
            const uint8_t *entryRuns = entryCtx + kCtxBytes;
            const uint8_t *outcalls = entryRuns + entryRunBytes;
            const uint8_t *exitCtx = outcalls + outcallBytes;
            const uint8_t *exitRuns = exitCtx + kCtxBytes;

            // The entry state: the chain's next step, applied to the chain and to memory (equal between samples).
            {
                const uint8_t *q = entryRuns;
                for (uint32_t i = 0; i < entryRunCount; ++i)
                {
                    const uint32_t address = rd32(q), len = rd32(q + 4);
                    std::memcpy(s.chain.data() + address, q + 8, len);
                    q += 8 + ((len + 3u) & ~3u);
                }
                applyRuns(s, entryRuns, entryRunCount);
            }

            ++total;
            R5900Context ctx;
            uint32_t words[kCtxWords];
            std::memcpy(words, entryCtx, kCtxBytes);
            fromRecord(words, ctx);
            ctx.pc = function;
            void *original = s.originals.count(function) ? s.originals[function] : nullptr;
            PS2Runtime::RecompiledFunction fn = original ? reinterpret_cast<PS2Runtime::RecompiledFunction>(original)
                                                         : runtime.lookupFunction(function);
            Replay &r = s.replay;
            r = Replay{};
            r.active = true;
            r.cur = outcalls;
            r.end = outcalls + outcallBytes;
            runtime.eeScheduler().clearTransferToDispatcher();
            std::string problem;
            const uint32_t stackPointer = rd32(p + 56);
            try
            {
                fn(s.ram, &ctx, &runtime);
                // A jump out through the dispatcher: run what it would until the call returns.
                for (uint32_t hops = 0; !(ctx.pc == returnAddress && getRegU32(&ctx, 29) == stackPointer) &&
                                        !r.runaway && hops < 1000000u;
                     ++hops)
                {
                    runtime.eeScheduler().clearTransferToDispatcher();
                    const uint32_t pc = ctx.pc;
                    if (isOverride(pc))
                    {
                        replayOut(s, &ctx, kOutOverride, pc);
                        continue;
                    }
                    if (!runtime.hasFunction(pc))
                        break;
                    runtime.lookupFunction(pc)(s.ram, &ctx, &runtime);
                }
            }
            catch (const Divergence &d)
            {
                problem = "diverged: " + d.what;
                ++diverged;
            }
            catch (const std::exception &e)
            {
                problem = std::string("exception: ") + e.what();
                ++diverged;
            }
            catch (...)
            {
                problem = "exception (a dispatcher transfer)";
                ++diverged;
            }
            r.active = false;
            if (problem.empty() && r.runaway)
            {
                problem = "runaway (no return after 400M checkpoints)";
                ++runaway;
            }
            if (problem.empty() && (ctx.pc != returnAddress || getRegU32(&ctx, 29) != stackPointer))
            {
                char text[120];
                std::snprintf(text, sizeof(text), "did not return: pc 0x%x, ra 0x%x", ctx.pc, returnAddress);
                problem = text;
                ++unwound;
            }
            if (problem.empty() && r.index != outcallCount)
            {
                char text[120];
                std::snprintf(text, sizeof(text), "%u of %u out-calls made", r.index, outcallCount);
                problem = text;
                ++unconsumed;
            }
            if (problem.empty())
            {
                uint32_t now[kCtxWords];
                toRecord(ctx, now);
                const uint32_t *expected = reinterpret_cast<const uint32_t *>(exitCtx);
                std::string differ;
                int count = 0;
                for (uint32_t i = 0; i < kCtxWords; ++i)
                {
                    uint32_t e;
                    std::memcpy(&e, expected + i, 4);
                    if (now[i] != e && count++ < 4)
                    {
                        char text[96];
                        std::snprintf(text, sizeof(text), " %s=0x%08x (capture 0x%08x)", wordName(i).c_str(), now[i], e);
                        differ += text;
                    }
                }
                if (count)
                {
                    problem = "context:" + differ + (count > 4 ? " ..." : "");
                    ++ctxBad;
                }
            }

            // Memory: the pages written during the replay and those the exit runs touch must equal the entry state
            // with the exit runs applied.
            collect(s);
            std::unordered_map<uint32_t, std::vector<uint8_t>> expected;
            const auto expectedPage = [&](uint32_t page) -> std::vector<uint8_t> &
            {
                auto it = expected.find(page);
                if (it == expected.end())
                {
                    const uint8_t *chainPage = s.chain.data() + size_t(page) * kPage;
                    it = expected.emplace(page, std::vector<uint8_t>(chainPage, chainPage + kPage)).first;
                }
                return it->second;
            };
            {
                const uint8_t *q = exitRuns;
                for (uint32_t i = 0; i < exitRunCount; ++i)
                {
                    const uint32_t address = rd32(q), len = rd32(q + 4);
                    for (uint32_t j = 0; j < len; ++j)
                        expectedPage((address + j) / kPage)[(address + j) % kPage] = q[8 + j];
                    q += 8 + ((len + 3u) & ~3u);
                }
            }
            for (uint32_t page : s.pages)
                expectedPage(page);
            if (problem.empty())
            {
                for (const auto &[page, bytes] : expected)
                {
                    const uint8_t *now = flatPage(s, page);
                    if (std::memcmp(now, bytes.data(), kPage) == 0)
                        continue;
                    uint32_t at = 0;
                    while (now[at] == bytes[at])
                        ++at;
                    char text[160];
                    std::snprintf(text, sizeof(text), "memory: flat 0x%07x is 0x%02x, the capture has 0x%02x",
                                  page * kPage + at, now[at], bytes[at]);
                    problem = text;
                    ++memBad;
                    break;
                }
            }
            // Back to the entry state (the chain) for the next sample.
            for (const auto &[page, bytes] : expected)
                std::memcpy(flatPage(s, page), s.chain.data() + size_t(page) * kPage, kPage);
            if (!s.watchBroken)
                watch::collect(s.ram, kRamBytes, s.pages.data(), kRamPages);

            if (problem.empty())
                ++matched;
            else
            {
                ++badFunctions[function];
                if (report)
                    std::fprintf(report, "sample %u function 0x%06x: %s\n", index, function, problem.c_str());
                if (logged++ < s.cfg.checkLog)
                    std::fprintf(stderr, "[capture-check] sample %u function 0x%06x: %s\n", index, function,
                                 problem.c_str());
            }
            if (total % 1000u == 0u)
                std::fprintf(stderr, "[capture-check] %" PRIu64 " samples, %" PRIu64 " match\n", total, matched);
        }
        std::fclose(in);
        char line[512];
        std::snprintf(line, sizeof(line),
                      "%" PRIu64 " samples replayed: %" PRIu64 " match bit for bit, %" PRIu64 " differ (%" PRIu64
                      " diverged at an out-call or threw, %" PRIu64 " context, %" PRIu64 " memory, %" PRIu64
                      " out-calls left, %" PRIu64 " did not return, %" PRIu64 " runaway) in %zu functions; %" PRIu64
                      " unreplayable calls recorded\n",
                      total, matched, total - matched, diverged, ctxBad, memBad, unconsumed, unwound, runaway,
                      badFunctions.size(), skips);
        std::fprintf(stderr, "[capture-check] %s", line);
        if (report)
        {
            std::fprintf(report, "%s", line);
            std::fclose(report);
        }
        std::fflush(nullptr);
        return total == matched ? 0 : 1;
    }
}
