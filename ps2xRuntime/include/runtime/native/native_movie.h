#ifndef PS2X_NATIVE_MOVIE_H
#define PS2X_NATIVE_MOVIE_H

// The movie pictures the MPEG decoder (Stubs/MPEG.cpp, standing in for the IPU) hands to the game at sceMpegGetPicture,
// for a port that shows them itself: the guest buffer it was written to, its size, and its pixels (RGBA, rows of
// width), before the game converts them into GS image transfers.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace ps2x::native
{
    using MoviePictureHook = void (*)(uint32_t guestImage, uint32_t width, uint32_t height, const uint8_t *rgba);

    inline std::atomic<MoviePictureHook> g_moviePictureHook{nullptr};
    inline void setMoviePictureHook(MoviePictureHook hook) { g_moviePictureHook.store(hook, std::memory_order_release); }
    inline MoviePictureHook moviePictureHook() { return g_moviePictureHook.load(std::memory_order_acquire); }

    // The picture a game shows through the display's read circuit 2 (PMODE EN2) under its frame, placed by the port
    // natively instead of uploaded to GS memory: TMB's shell menus are drawn with alpha 0 where the movie frame under
    // them shows. RGBA rows of width, alpha as the GS stores it (0x80 = 1.0); width x height covers the displayed area
    // from its top left. Version changes with the pixels; null pixels: none (circuit 2 read from GS memory again).
    struct DisplayPicture
    {
        std::shared_ptr<const std::vector<uint32_t>> rgba;
        uint32_t width = 0, height = 0;
        uint64_t version = 0;
    };
    inline std::mutex g_displayPictureMutex;
    inline DisplayPicture g_displayPicture;
    inline void setDisplayPicture(std::shared_ptr<const std::vector<uint32_t>> rgba, uint32_t width, uint32_t height)
    {
        std::lock_guard<std::mutex> lock(g_displayPictureMutex);
        g_displayPicture.rgba = std::move(rgba);
        g_displayPicture.width = width;
        g_displayPicture.height = height;
        ++g_displayPicture.version;
    }
    inline void clearDisplayPicture() { setDisplayPicture(nullptr, 0u, 0u); }
    inline DisplayPicture displayPicture()
    {
        std::lock_guard<std::mutex> lock(g_displayPictureMutex);
        return g_displayPicture;
    }
}

#endif
