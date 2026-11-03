// The capture's write tracking of main RAM (ps2_capture.cpp): on Windows, the RAM allocated with MEM_WRITE_WATCH and
// the pages written since the last call read with GetWriteWatch. On its own (outside the unity batches): <windows.h>
// clashes with raylib's names.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ps2x::capture::watch
{
    // Zeroed memory whose written pages can be listed (nullptr when the platform cannot).
    uint8_t *allocate(size_t bytes)
    {
#if defined(_WIN32)
        void *memory = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        return static_cast<uint8_t *>(memory);
#else
        (void)bytes;
        return nullptr;
#endif
    }

    void release(uint8_t *memory)
    {
#if defined(_WIN32)
        if (memory)
            VirtualFree(memory, 0, MEM_RELEASE);
#else
        (void)memory;
#endif
    }

    // The 4 KB pages of [base, base + bytes) written since the last call (their indices), the tracking reset.
    // Returns the count, or SIZE_MAX when the pages are not known (every page must then be taken as written).
    size_t collect(uint8_t *base, size_t bytes, uint32_t *pages, size_t maxPages)
    {
#if defined(_WIN32)
        static thread_local void **s_addresses = nullptr;
        static thread_local size_t s_capacity = 0;
        if (s_capacity < maxPages)
        {
            std::free(s_addresses);
            s_addresses = static_cast<void **>(std::malloc(maxPages * sizeof(void *)));
            s_capacity = maxPages;
        }
        ULONG_PTR count = maxPages;
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, bytes, s_addresses, &count, &granularity) != 0 ||
            granularity != 4096u)
            return SIZE_MAX;
        for (ULONG_PTR i = 0; i < count; ++i)
            pages[i] = static_cast<uint32_t>((static_cast<uint8_t *>(s_addresses[i]) - base) / 4096u);
        return static_cast<size_t>(count);
#else
        (void)base;
        (void)bytes;
        (void)pages;
        (void)maxPages;
        return SIZE_MAX;
#endif
    }
}
