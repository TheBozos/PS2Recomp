#ifndef PS2_ADDRESS_H
#define PS2_ADDRESS_H

#include <cstdint>

#include "runtime/ps2_memory_map.h"

// The guest's loads and stores go through these on every access: forced inline, as GCC stops inlining in the very
// large generated functions and each access then paid a call.
#ifndef PS2_ALWAYS_INLINE // (a build can define it as plain inline to compare)
#if defined(_MSC_VER)
#define PS2_ALWAYS_INLINE __forceinline
#else
#define PS2_ALWAYS_INLINE inline __attribute__((always_inline))
#endif
#endif

static inline constexpr uint32_t PS2_EE_UNCACHED_RAM_MIRROR_BASE = 0x20000000u;
static inline constexpr uint32_t PS2_EE_UNCACHED_RAM_MIRROR_SIZE = 0x20000000u;
static inline constexpr uint32_t PS2_KSEG0_BASE = 0x80000000u;
static inline constexpr uint32_t PS2_KSEG0_KSEG1_SIZE = 0x40000000u;
static inline constexpr uint32_t PS2_KSEG2_BASE = 0xC0000000u;

static inline constexpr bool Ps2AddressInRange(uint32_t value, uint32_t base, uint32_t size)
{
    return (value - base) < size;
}

static inline constexpr bool Ps2IsUncachedRamMirrorAddress(uint32_t addr)
{
    return Ps2AddressInRange(addr, PS2_EE_UNCACHED_RAM_MIRROR_BASE, PS2_EE_UNCACHED_RAM_MIRROR_SIZE);
}

static inline constexpr bool Ps2IsKseg01Address(uint32_t addr)
{
    return Ps2AddressInRange(addr, PS2_KSEG0_BASE, PS2_KSEG0_KSEG1_SIZE);
}

static inline constexpr bool Ps2IsKseg23Address(uint32_t addr)
{
    return addr >= PS2_KSEG2_BASE;
}

static inline constexpr uint32_t Ps2DirectMappedPhysicalAddress(uint32_t addr)
{
    return addr & 0x1FFFFFFFu;
}

static inline constexpr uint32_t Ps2PhysicalAddress(uint32_t addr)
{
    return (addr >= PS2_KSEG0_BASE) ? Ps2DirectMappedPhysicalAddress(addr) : addr;
}

static inline constexpr bool Ps2IsPhysicalSpecialAddress(uint32_t physAddr)
{
    return Ps2AddressInRange(physAddr, PS2_BIOS_BASE, PS2_BIOS_SIZE) ||
           Ps2AddressInRange(physAddr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE) ||
           Ps2AddressInRange(physAddr, PS2_IO_BASE, PS2_IO_SIZE) ||
           Ps2AddressInRange(physAddr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE) ||
           // The VU0/VU1 micro and data memories (0x11000000-0x1100FFFF) are not RDRAM.
           (physAddr >= PS2_VU0_CODE_BASE && physAddr < (PS2_VU1_DATA_BASE + PS2_VU1_DATA_SIZE));
}

static PS2_ALWAYS_INLINE constexpr bool Ps2IsSpecialAddress(uint32_t addr)
{
    // Main RAM (kuseg 0 to 32 MB): no special region lies there, and it is where nearly every access goes.
    if (addr < PS2_RAM_SIZE)
        return false;
    if (Ps2IsKseg23Address(addr))
        return true;

    return Ps2IsPhysicalSpecialAddress(Ps2PhysicalAddress(addr));
}

#endif // PS2_ADDRESS_H
