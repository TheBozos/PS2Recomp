#ifndef PS2X_CD_READ_OBSERVER_H
#define PS2X_CD_READ_OBSERVER_H

#include <cstdint>

// A port told of every disc read the game asks for (sceCdRead: the sector, the sector count, the EE buffer), before it
// is done: to know which file a buffer holds (a game's own assets found by where they came from on the disc).
namespace ps2x
{
    using CdReadObserver = void (*)(uint32_t lbn, uint32_t sectors, uint32_t buffer);
    void setCdReadObserver(CdReadObserver observer);
}

#endif
