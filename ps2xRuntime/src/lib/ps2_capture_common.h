// Internal to the per-function capture (ps2_capture.cpp) and the call log (ps2_calllog.cpp): the pieces of their file
// formats they share (BozoEngine's docs/specs/capture.md and calllog.md): the flat space, the context record, byte
// runs, the out-call kinds, and the write tracking of main RAM.

#ifndef PS2_CAPTURE_COMMON_H
#define PS2_CAPTURE_COMMON_H

#include "ps2_recomp_abi.h"
#include "runtime/ps2_memory_map.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ps2x::capture
{
    namespace watch
    {
        uint8_t *allocate(size_t bytes);
        void release(uint8_t *memory);
        size_t collect(uint8_t *base, size_t bytes, uint32_t *pages, size_t maxPages);
    }

    namespace common
    {
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
        constexpr uint32_t kOutHeaderBytes = 36;

        constexpr uint32_t tag(const char (&s)[5])
        {
            return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
                   uint32_t(uint8_t(s[3])) << 24;
        }

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

        inline uint32_t bitsOf(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        inline float floatOf(uint32_t u)
        {
            float f;
            std::memcpy(&f, &u, 4);
            return f;
        }
        inline uint32_t rd32(const uint8_t *p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        inline uint64_t rd64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }

        // The context record (capture.md, "Context record").
        inline void toRecord(const R5900Context &c, uint32_t *w)
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

        inline void fromRecord(const uint32_t *w, R5900Context &c)
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

        inline std::string wordName(uint32_t i)
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
        inline uint32_t diffPage(const uint8_t *before, const uint8_t *after, uint32_t base, Buf &out)
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

        inline const uint8_t *skipRuns(const uint8_t *p, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t len = rd32(p + 4);
                p += 8 + ((len + 3u) & ~3u);
            }
            return p;
        }

        // The context words that differ between two records: the mask (kMaskWords words) and the new values in order.
        inline uint32_t diffContext(const uint32_t *before, const uint32_t *after, uint32_t *mask,
                                    std::vector<uint32_t> &words)
        {
            std::memset(mask, 0, kMaskWords * 4u);
            words.clear();
            for (uint32_t i = 0; i < kCtxWords; ++i)
                if (before[i] != after[i])
                {
                    mask[i / 32] |= 1u << (i % 32);
                    words.push_back(after[i]);
                }
            return static_cast<uint32_t>(words.size());
        }

        inline void applyContextWords(const uint32_t *mask, const uint32_t *words, uint32_t *record)
        {
            uint32_t k = 0;
            for (uint32_t i = 0; i < kCtxWords; ++i)
                if (mask[i / 32] & (1u << (i % 32)))
                    record[i] = words[k++];
        }
    }
}

#endif // PS2_CAPTURE_COMMON_H
