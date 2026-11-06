// The capture's write tracking of main RAM (ps2_capture.cpp): on Windows, the RAM allocated with MEM_WRITE_WATCH and
// the pages written since the last call read with GetWriteWatch; on Linux, the same through asynchronous userfaultfd
// write protection and the PAGEMAP_SCAN ioctl (6.7 or later), which also sees what the kernel writes (a file read into
// RAM). On its own (outside the unity batches): <windows.h> clashes with raylib's names.

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
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#endif

namespace ps2x::capture::watch
{
#if defined(__linux__) && defined(PAGEMAP_SCAN)
    static int s_pagemap = -1;
#endif

    // Zeroed memory whose written pages can be listed (nullptr when the platform cannot).
    uint8_t *allocate(size_t bytes)
    {
#if defined(_WIN32)
        void *memory = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        return static_cast<uint8_t *>(memory);
#elif defined(__linux__) && defined(PAGEMAP_SCAN)
        const int uffd = static_cast<int>(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        s_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (uffd < 0 || s_pagemap < 0 || memory == MAP_FAILED)
            return nullptr;
        madvise(memory, bytes, MADV_NOHUGEPAGE);
        std::memset(memory, 0, bytes);
        uffdio_api api{};
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_WP_ASYNC;
        uffdio_register reg{};
        reg.range = {reinterpret_cast<uintptr_t>(memory), bytes};
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        uffdio_writeprotect wp{};
        wp.range = reg.range;
        wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_API, &api) != 0 || ioctl(uffd, UFFDIO_REGISTER, &reg) != 0 || ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) != 0)
        {
            munmap(memory, bytes);
            return nullptr;
        }
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
#elif defined(__linux__) && defined(PAGEMAP_SCAN)
        static thread_local std::vector<page_region> s_regions(1024u);
        pm_scan_arg arg{};
        arg.size = sizeof(arg);
        arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        arg.start = reinterpret_cast<uintptr_t>(base);
        arg.end = arg.start + bytes;
        arg.vec = reinterpret_cast<uintptr_t>(s_regions.data());
        arg.vec_len = s_regions.size();
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;
        size_t count = 0;
        while (arg.start < arg.end)
        {
            const long regions = ioctl(s_pagemap, PAGEMAP_SCAN, &arg);
            if (regions < 0)
                return SIZE_MAX;
            for (long r = 0; r < regions; ++r)
                for (uint64_t at = s_regions[r].start; at < s_regions[r].end && count < maxPages; at += 4096u)
                    pages[count++] = static_cast<uint32_t>((at - reinterpret_cast<uintptr_t>(base)) / 4096u);
            arg.start = arg.walk_end;
            if (regions < static_cast<long>(s_regions.size()))
                break;
        }
        return count;
#else
        (void)base;
        (void)bytes;
        (void)pages;
        (void)maxPages;
        return SIZE_MAX;
#endif
    }
}
