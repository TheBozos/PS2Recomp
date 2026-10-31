#ifndef PS2X_GS_OFF_H
#define PS2X_GS_OFF_H

// No GS at all (PS2X_GS_RENDERER=none, for ports whose picture is entirely native): the GS front end neither draws,
// keeps nor scans GIF packets; it only answers the game's waits (SIGNAL, FINISH, LABEL raised at once), and a
// download from GS memory reads zeros. The window shows the native renderer alone.
namespace ps2x
{
    void setGsOff(bool off);
    bool gsOff();
}

#endif
