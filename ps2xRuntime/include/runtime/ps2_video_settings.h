#pragma once

// The window as the player sets it: its size, windowed / fullscreen / borderless, and the render scales. Loaded from
// video.ini next to the game's ELF (or PS2X_VIDEO_CONFIG) when the window opens, edited in the Video tab of the
// settings window (F2) and saved back there; the environment still overrides the file (PS2X_WINDOW_SIZE,
// PS2X_WINDOW_MODE, PS2X_RENDER_SCALE, PS2X_GS_SCALE, PS2X_FRAME_RATE). F11 or Alt+Enter switch between the window and the last full-screen
// mode. Whatever the window's shape, the picture keeps its 4:3 and is centred with bars (the present loop).
//
// Everything here runs on the window's thread.

#include <string>
#include <utility>
#include <vector>

namespace ps2_video
{
    enum class WindowMode : int
    {
        Windowed,
        Fullscreen, // exclusive: the monitor switches to the chosen size
        Borderless, // a window without borders over the whole monitor, at the monitor's own size
    };

    struct Settings
    {
        int width = 0, height = 0; // the window's client area (the full-screen video mode's size)
        WindowMode mode = WindowMode::Windowed;
        float renderScale = 1.0f; // the native renderer's 3D against the window's size (live)
        int gsScale = 1;          // the GS renderer's internal scale, 1 to 4 (PS2X_GS_SCALE; at the next launch)
        // The window's frame rate (live): 0 the game's 60 (the default), -1 the monitor's refresh with vsync, n that many
        // a second without vsync; above the game's, the native renderer draws frames between the game's
        // (native::Renderer::setFrameRate).
        int frameRate = 0;
    };

    // frame_rate / PS2X_FRAME_RATE: "game", "monitor" or a number (false for anything else).
    bool parseFrameRate(const char *text, int &rate);
    std::string frameRateName(int rate);

    // Before InitWindow: the file then the environment over the defaults (the runner's window size). The ELF's folder
    // must be known by then (PS2Runtime::configureIoPathsFromElf).
    void loadAtStartup(int defaultWidth, int defaultHeight);
    // After InitWindow: the mode and render scale loaded.
    void applyAtStartup();
    // Once a frame, before drawing it (window thread): F11 / Alt+Enter, requestApply()'s change, and a windowed resize kept
    // as the size.
    void frameUpdate();

    // What is applied now, and the settings window's copy of it until Apply.
    const Settings &current();
    Settings &edited();
    // Applies `edited()` to the window at once (the GS scale waits for the next launch); requestApply() at the next
    // frameUpdate (from inside a frame: the settings window).
    void apply();
    void requestApply();
    bool save();
    std::string configPath();

    const char *modeName(WindowMode mode);
    // Sizes to offer: common 4:3 and 16:9 ones that fit the monitor, the monitor's own, and the current one.
    std::vector<std::pair<int, int>> resolutions();
    std::pair<int, int> monitorSize();
}
