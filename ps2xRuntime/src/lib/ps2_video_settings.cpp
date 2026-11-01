#include "runtime/ps2_video_settings.h"

#include "ps2_runtime.h"
#include "ps2_app.h"
#include "runtime/native/native_renderer.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace ps2_video
{
    namespace
    {
        Settings g_current;
        Settings g_edited;
        WindowMode g_lastFull = WindowMode::Borderless; // where F11 / Alt+Enter go from the window
        // The window's position before it last left windowed mode, to come back there (-1: centred).
        int g_windowX = -1, g_windowY = -1;
        bool g_applyPending = false; // requestApply(): at the next frameUpdate, between two frames

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
        }

        bool parseMode(std::string_view text, WindowMode &mode)
        {
            if (text == "windowed")
                mode = WindowMode::Windowed;
            else if (text == "fullscreen")
                mode = WindowMode::Fullscreen;
            else if (text == "borderless")
                mode = WindowMode::Borderless;
            else
                return false;
            return true;
        }

        bool parseSize(const char *text, int &width, int &height)
        {
            int w = 0, h = 0;
            if (std::sscanf(text, "%dx%d", &w, &h) != 2 || w < 160 || h < 120)
                return false;
            width = w;
            height = h;
            return true;
        }

        void sanitize(Settings &settings)
        {
            settings.renderScale = std::clamp(settings.renderScale, 0.25f, 4.0f);
            settings.gsScale = std::clamp(settings.gsScale, 1, 4);
        }

        void load(Settings &settings)
        {
            std::ifstream file(configPath());
            if (!file)
                return;
            std::string line;
            while (std::getline(file, line))
            {
                line = trim(line.substr(0, line.find('#')));
                const size_t equal = line.find('=');
                if (line.empty() || line.front() == '[' || equal == std::string::npos)
                    continue;
                const std::string name = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
                if (name == "size")
                    parseSize(value.c_str(), settings.width, settings.height);
                else if (name == "mode")
                    parseMode(value, settings.mode);
                else if (name == "render_scale")
                    settings.renderScale = static_cast<float>(std::atof(value.c_str()));
                else if (name == "gs_scale")
                    settings.gsScale = std::atoi(value.c_str());
            }
        }

        void centreWindow(int width, int height)
        {
            const int monitor = GetCurrentMonitor();
            const Vector2 origin = GetMonitorPosition(monitor);
            SetWindowPosition(static_cast<int>(origin.x) + std::max(0, (GetMonitorWidth(monitor) - width) / 2),
                              static_cast<int>(origin.y) + std::max(0, (GetMonitorHeight(monitor) - height) / 2));
        }

        // Back to a plain window from either full-screen mode (raylib restores the size and place it had).
        void leaveFullModes()
        {
            if (IsWindowFullscreen())
                ToggleFullscreen();
            if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
                ToggleBorderlessWindowed();
        }

        void applyWindow(const Settings &from, const Settings &to)
        {
            if (from.mode == WindowMode::Windowed && to.mode != WindowMode::Windowed)
            {
                const Vector2 position = GetWindowPosition();
                g_windowX = static_cast<int>(position.x);
                g_windowY = static_cast<int>(position.y);
            }
            leaveFullModes();
            // The size first, as a window: exclusive full screen then asks the monitor for that size (ToggleFullscreen
            // keeps the window's), borderless covers the monitor whatever it is.
            if (GetScreenWidth() != to.width || GetScreenHeight() != to.height)
                SetWindowSize(to.width, to.height);
            switch (to.mode)
            {
            case WindowMode::Windowed:
                if (from.mode != WindowMode::Windowed || from.width != to.width || from.height != to.height)
                {
                    if (from.mode != WindowMode::Windowed && g_windowX >= 0 && from.width == to.width && from.height == to.height)
                        SetWindowPosition(g_windowX, g_windowY);
                    else
                        centreWindow(to.width, to.height);
                }
                break;
            case WindowMode::Fullscreen:
                ToggleFullscreen();
                break;
            case WindowMode::Borderless:
                ToggleBorderlessWindowed();
                break;
            }
            if (to.mode != WindowMode::Windowed)
                g_lastFull = to.mode;
        }
    }

    const char *modeName(WindowMode mode)
    {
        switch (mode)
        {
        case WindowMode::Fullscreen:
            return "fullscreen";
        case WindowMode::Borderless:
            return "borderless";
        default:
            return "windowed";
        }
    }

    std::string configPath()
    {
        if (const char *path = std::getenv("PS2X_VIDEO_CONFIG"); path && *path)
            return path;
        // The per-user folder of a port that has one (ps2_app.h), else the ELF's.
        const std::filesystem::path dir =
            ps2x::app::userDirectory().empty() ? PS2Runtime::getIoPaths().elfDirectory : ps2x::app::userDirectory();
        return (dir.empty() ? std::filesystem::path("video.ini") : dir / "video.ini").string();
    }

    void loadAtStartup(int defaultWidth, int defaultHeight)
    {
        Settings settings;
        settings.width = defaultWidth;
        settings.height = defaultHeight;
        load(settings);
        // The environment over the file.
        if (const char *size = std::getenv("PS2X_WINDOW_SIZE"); size && *size)
            parseSize(size, settings.width, settings.height);
        if (const char *mode = std::getenv("PS2X_WINDOW_MODE"); mode && *mode)
            parseMode(mode, settings.mode);
        if (const char *scale = std::getenv("PS2X_RENDER_SCALE"); scale && *scale)
            settings.renderScale = static_cast<float>(std::atof(scale));
        if (const char *scale = std::getenv("PS2X_GS_SCALE"); scale && *scale)
            settings.gsScale = std::atoi(scale);
        sanitize(settings);
        // The GS back end reads its scale from the environment when it starts.
        if (settings.gsScale != 1 && !std::getenv("PS2X_GS_SCALE"))
        {
            const std::string value = std::to_string(settings.gsScale);
#if defined(_WIN32)
            _putenv_s("PS2X_GS_SCALE", value.c_str());
#else
            setenv("PS2X_GS_SCALE", value.c_str(), 0);
#endif
        }
        g_current = settings;
        g_current.mode = WindowMode::Windowed; // InitWindow opens a window; applyAtStartup goes on from there
        g_edited = settings;
    }

    void applyAtStartup()
    {
        Settings target = g_edited;
        if (target.mode != WindowMode::Windowed)
            applyWindow(g_current, target);
        ps2x::native::Renderer::instance().setRenderScale(target.renderScale);
        g_current = target;
        std::fprintf(stderr, "[video] %dx%d %s, render scale %.2f, GS scale %d (%s)\n", g_current.width, g_current.height,
                     modeName(g_current.mode), g_current.renderScale, g_current.gsScale, configPath().c_str());
    }

    void frameUpdate()
    {
        // A windowed resize by hand is the window's size from then on (saved with the rest).
        if (IsWindowResized() && g_current.mode == WindowMode::Windowed && !IsWindowFullscreen() &&
            !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        {
            g_current.width = GetScreenWidth();
            g_current.height = GetScreenHeight();
            g_edited.width = g_current.width;
            g_edited.height = g_current.height;
        }
        if (IsKeyPressed(KEY_F11) || ((IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT)) && IsKeyPressed(KEY_ENTER)))
        {
            g_edited = g_current;
            g_edited.mode = g_current.mode == WindowMode::Windowed ? g_lastFull : WindowMode::Windowed;
            g_applyPending = true;
        }
        // PS2X_VIDEO_SCRIPT=<s>:<w>x<h>:<mode>[:<render scale>],...: changes applied at those seconds from the first
        // frame, as the Video tab would (unattended checks of the mode switches).
        static std::vector<std::pair<double, Settings>> s_script = []
        {
            std::vector<std::pair<double, Settings>> steps;
            const char *text = std::getenv("PS2X_VIDEO_SCRIPT");
            for (std::string rest = text ? text : ""; !rest.empty();)
            {
                const size_t comma = rest.find(',');
                const std::string step = rest.substr(0, comma);
                rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                double at = 0.0;
                int w = 0, h = 0;
                char mode[16] = {};
                float scale = 0.0f;
                if (std::sscanf(step.c_str(), "%lf:%dx%d:%15[a-z]:%f", &at, &w, &h, mode, &scale) < 4)
                    continue;
                Settings settings;
                settings.width = w;
                settings.height = h;
                settings.renderScale = scale;
                if (parseMode(mode, settings.mode))
                    steps.emplace_back(at, settings);
            }
            return steps;
        }();
        static const double s_start = GetTime();
        while (!s_script.empty() && GetTime() - s_start >= s_script.front().first)
        {
            const Settings &step = s_script.front().second;
            g_edited = g_current;
            g_edited.width = step.width;
            g_edited.height = step.height;
            g_edited.mode = step.mode;
            if (step.renderScale > 0.0f)
                g_edited.renderScale = step.renderScale;
            s_script.erase(s_script.begin());
            g_applyPending = true;
        }
        if (g_applyPending)
        {
            g_applyPending = false;
            apply();
        }
    }

    void requestApply() { g_applyPending = true; }

    const Settings &current() { return g_current; }
    Settings &edited() { return g_edited; }

    void apply()
    {
        sanitize(g_edited);
        if (g_edited.mode != g_current.mode || g_edited.width != g_current.width || g_edited.height != g_current.height)
            applyWindow(g_current, g_edited);
        ps2x::native::Renderer::instance().setRenderScale(g_edited.renderScale);
        g_current = g_edited;
        std::fprintf(stderr, "[video] applied %dx%d %s, render scale %.2f (window %dx%d)\n", g_current.width, g_current.height,
                     modeName(g_current.mode), g_current.renderScale, GetScreenWidth(), GetScreenHeight());
    }

    bool save()
    {
        std::ofstream file(configPath(), std::ios::trunc);
        if (!file)
            return false;
        const Settings &s = g_edited;
        char scale[32];
        std::snprintf(scale, sizeof(scale), "%g", s.renderScale);
        file << "# The window (the Video tab of the settings window, F2, writes this file; PS2X_WINDOW_SIZE, PS2X_WINDOW_MODE,\n"
                "# PS2X_RENDER_SCALE and PS2X_GS_SCALE override it). size: the window's, or the full-screen video mode's.\n"
                "# mode: windowed, fullscreen (exclusive) or borderless (the monitor's size). render_scale: the 3D drawn at\n"
                "# that many times the window's size each way (0.25 to 4). gs_scale: the GS renderer's internal scale (1 to 4,\n"
                "# read at launch).\n"
                "\n[video]\n"
             << "size = " << s.width << "x" << s.height << "\n"
             << "mode = " << modeName(s.mode) << "\n"
             << "render_scale = " << scale << "\n"
             << "gs_scale = " << s.gsScale << "\n";
        return static_cast<bool>(file);
    }

    std::pair<int, int> monitorSize()
    {
        const int monitor = GetCurrentMonitor();
        return {GetMonitorWidth(monitor), GetMonitorHeight(monitor)};
    }

    std::vector<std::pair<int, int>> resolutions()
    {
        static const std::pair<int, int> kCommon[] = {
            {640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2880, 2160}, // 4:3
            {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160},           // 16:9
        };
        const auto [monitorWidth, monitorHeight] = monitorSize();
        std::vector<std::pair<int, int>> sizes;
        for (const auto &size : kCommon)
            if (monitorWidth <= 0 || (size.first <= monitorWidth && size.second <= monitorHeight))
                sizes.push_back(size);
        if (monitorWidth > 0)
            sizes.emplace_back(monitorWidth, monitorHeight);
        sizes.emplace_back(g_current.width, g_current.height);
        sizes.emplace_back(g_edited.width, g_edited.height);
        std::sort(sizes.begin(), sizes.end());
        sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
        return sizes;
    }
}
