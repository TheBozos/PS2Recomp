#ifndef PS2X_DISPLAY_PHASE_H
#define PS2X_DISPLAY_PHASE_H

// Where "now" falls in the PC display's refresh, for a game on native frame pacing (frames on the PC clock, at the
// display's rate). Its deadlines stay on the PC clock; only their phase is steered, so that a frame is published half
// a refresh away from the moment the compositor takes the window's picture. Otherwise, with the game and the display
// both at 60 Hz, frames published near that moment go to one refresh or the next by a millisecond of the frame's own
// work, and one frame shows twice while the next is never seen (a stutter). Nothing waits on the display.
namespace ps2x
{
    // Seconds from the middle of the current refresh to now, in (-period / 2, period / 2]; false where the display's
    // timing is not known (no compositor timing: not Windows, or composition off).
    bool displayPhaseError(double &seconds);
}

#endif
