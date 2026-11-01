#include "runtime/ps2_debug_ui.h"

#include <mutex>

namespace ps2x::debugui
{
    namespace
    {
        std::mutex g_mutex;
        std::vector<Tab> g_tabs;
    }

    void registerTab(const std::string &name, std::function<void()> draw)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Tab &tab : g_tabs)
            if (tab.name == name)
            {
                tab.draw = std::move(draw);
                return;
            }
        g_tabs.push_back(Tab{name, std::move(draw)});
    }

    std::vector<Tab> tabs()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_tabs;
    }
}
