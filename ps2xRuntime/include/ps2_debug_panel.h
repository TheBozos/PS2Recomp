#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

#include <string>

class PS2Runtime;

class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Runtime &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }

private:
    // The settings window (F2): the pad bindings (runtime/ps2_input_bindings.h), the window and rendering
    // (runtime/ps2_video_settings.h), then the port's own tabs (runtime/ps2_debug_ui.h).
    void drawSettingsWindow();
    void drawControlsTab();
    void drawVideoTab();

    bool m_initialized = false;
    bool m_visible = true;
    bool m_controlsVisible = false;
    std::string m_openTab; // PS2X_SETTINGS_OPEN: the settings tab to select on the first frame
    int m_controlsPlayer = 0;
    int m_bindingAction = -1; // the action waiting for an input in the Controls window
    bool m_debugExpanded = false;
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
