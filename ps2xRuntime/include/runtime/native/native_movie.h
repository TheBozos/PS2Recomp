#ifndef PS2X_NATIVE_MOVIE_H
#define PS2X_NATIVE_MOVIE_H

// The movie pictures the MPEG decoder (Stubs/MPEG.cpp, standing in for the IPU) hands to the game at sceMpegGetPicture,
// for a port that shows them itself: the guest buffer it was written to, its size, and its pixels (RGBA, rows of
// width), before the game converts them into GS image transfers.

#include <atomic>
#include <cstdint>

namespace ps2x::native
{
    using MoviePictureHook = void (*)(uint32_t guestImage, uint32_t width, uint32_t height, const uint8_t *rgba);

    inline std::atomic<MoviePictureHook> g_moviePictureHook{nullptr};
    inline void setMoviePictureHook(MoviePictureHook hook) { g_moviePictureHook.store(hook, std::memory_order_release); }
    inline MoviePictureHook moviePictureHook() { return g_moviePictureHook.load(std::memory_order_acquire); }
}

#endif
