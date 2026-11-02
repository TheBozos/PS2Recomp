#ifndef PS2_RUNTIME_DETERMINISM_H
#define PS2_RUNTIME_DETERMINISM_H

// PS2X_DETERMINISTIC=1: two runs with the same inputs compute the same guest state, frame for frame (the reference
// harness: ps2_reference.h records it). Nothing the guest can observe comes from the host's clock, its devices or its
// threads' timing:
// - the frame step the game's loop is given (FramePacer::frameEnd) is exactly one field (1 / the cap), whatever the
//   frame took; the window may still be paced to real time, which changes nothing computed;
// - EE time follows the cycles the guest accounts only: no wall-clock catch-up (PS2X_EE_CATCHUP), events due by
//   their cycle alone (no host deadlines; vblank holds ignored), idle waits charge exactly the cycles to the next event;
// - the IOP runs on the EE thread on the EE's cycles only (PS2X_IOP_WALLCLOCK and PS2X_IOP_THREAD ignored);
// - the native mixer is clocked by the IOP (768 IOP cycles per 48 kHz sample, mixed on the IOP's thread up to the
//   current IOP cycle before each libsd call and at the end of each IOP slice); the audio device only drains its
//   queue (silence when it runs dry); the emulated SPU2 is never on the wall clock;
// - the real-time clock (sceCdReadClock), memory card and file dates are fixed (fixedDate), the time zone is UTC;
// - pad scripts are keyed on game frames (frame:BUTTON/frames), no host keyboard, gamepad or gamepad presence;
// - movies are clocked by the vblanks (EE cycles), never by host deadlines, decode time not charged.
// Off (the default), every path behaves as before.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>

namespace ps2x::det
{
    inline bool enabled() noexcept
    {
        static const bool s_on = []
        {
            const char *value = std::getenv("PS2X_DETERMINISTIC");
            return value && *value == '1';
        }();
        return s_on;
    }

    // The date every clock the guest reads gives in deterministic mode: 2001-06-18 12:00:00 (TMB's North American
    // release), as local time and UTC alike.
    inline std::tm fixedDate() noexcept
    {
        std::tm tm{};
        tm.tm_year = 2001 - 1900;
        tm.tm_mon = 5;
        tm.tm_mday = 18;
        tm.tm_hour = 12;
        tm.tm_min = 0;
        tm.tm_sec = 0;
        tm.tm_wday = 1;
        tm.tm_yday = 168;
        return tm;
    }

    // The game frames ended so far (EeScheduler::presentGuestFrame, the game handing its picture over): frame 0 runs
    // until the first one ends. The pad scripts' clock in deterministic mode and the reference records' frame number.
    inline std::atomic<uint64_t> &frameCounter() noexcept
    {
        static std::atomic<uint64_t> s_frames{0};
        return s_frames;
    }
    inline uint64_t frame() noexcept { return frameCounter().load(std::memory_order_relaxed); }
}

#endif // PS2_RUNTIME_DETERMINISM_H
