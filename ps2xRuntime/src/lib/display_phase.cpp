#include "runtime/display_phase.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <cmath>

namespace ps2x
{
    bool displayPhaseError(double &seconds)
    {
        // The compositor's last vblank and refresh period, in QueryPerformanceCounter ticks (the PC clock).
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)) || timing.qpcRefreshPeriod == 0u)
            return false;
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double period = static_cast<double>(timing.qpcRefreshPeriod);
        double phase = std::fmod(static_cast<double>(now.QuadPart) - static_cast<double>(timing.qpcVBlank), period);
        if (phase < 0.0)
            phase += period;
        double error = phase - period / 2.0;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        seconds = error / static_cast<double>(frequency.QuadPart);
        return true;
    }
}
#else
namespace ps2x
{
    bool displayPhaseError(double &) { return false; }
}
#endif
