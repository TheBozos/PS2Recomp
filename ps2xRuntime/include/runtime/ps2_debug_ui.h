#pragma once

// Tabs a port adds to the settings window (F2) beside Controls and Video: a name and a callback drawing ImGui widgets,
// called on the window's thread each frame the tab is open (between ImGui::BeginTabItem and EndTabItem). Register
// from the port's overrides (any thread, before or after the window opens); without the debug UI
// (PS2X_ENABLE_DEBUG_UI) the tabs are kept and never drawn, so the port's callback guards its ImGui code with that
// define.

#include <functional>
#include <string>
#include <vector>

namespace ps2x::debugui
{
    struct Tab
    {
        std::string name;
        std::function<void()> draw;
    };

    // A second registration under the same name replaces the first.
    void registerTab(const std::string &name, std::function<void()> draw);
    // A copy, in registration order.
    std::vector<Tab> tabs();
}
