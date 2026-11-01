#ifndef PS2_VIF1_COMMAND_STATS_H
#define PS2_VIF1_COMMAND_STATS_H

#include <cstdint>

namespace ps2_vif1_command_stats
{
    void noteCommand(uint8_t opcode);
    uint64_t commands(uint8_t opcode);
    // Bytes of the VIF1 stream each command took (its code word and its data), DIRECT counting the data it hands the GIF.
    void noteBytes(uint8_t opcode, uint64_t bytes);
    uint64_t bytes(uint8_t opcode);
}

#endif
