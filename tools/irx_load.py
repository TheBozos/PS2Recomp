#!/usr/bin/env python3
"""Loads an IOP module (IRX) offline the way the runtime's IOP loads it (ps2xIOP IopModuleLoader::load): the PT_LOAD
segment (or the allocated sections) copied at the base, the bss zeroed, the relocations applied (R_MIPS_32 / REL32 /
26 / HI16 / LO16 / 16, the Sony IOPRELEXEC rule: image offsets from the base, symbol 0 the base), entry and gp from the
IOPMOD header (or REGINFO). It writes the pair PS2X_IOP_DUMP_MODULES writes when the game loads the module,
<name>_<base>.bin (the image, relocated, before its start routine) and <name>_<base>.json (base, size, entry, gp, the
file's FNV-1a 64 hash), the input of tools/iop_recomp.py, without running the game.

The base is the one the runtime gives the module (in load order, after the heap blocks the earlier start routines
allocated): the port records it (a module's native code is only activated at its base).

  tools/irx_load.py <file.irx> --base 0x52200 [--name 989SND.IRX;1] --out <dir>

Standard library only.
"""
import argparse
import json
import os
import struct
import sys

ET_EXEC = 2
ET_SCE_IOPRELEXEC = 0xFF80
ET_SCE_IOPRELEXEC2 = 0xFF81
EM_MIPS = 8
PT_LOAD = 1
PT_SCE_IOPMOD = 0x70000080
PT_MIPS_REGINFO = 0x70000000
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHT_MIPS_REGINFO = 0x70000006
SHF_ALLOC = 0x2
MODULE_LOAD_BASE = 0x00010000
RAM_SIZE = 2 * 1024 * 1024


class IrxError(Exception):
    pass


def fnv1a64(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def align_up(value, alignment):
    if alignment <= 1:
        return value
    return (value + alignment - 1) & ~(alignment - 1)


def load(image, base):
    """Returns dict(base, size, entry, gp, image=bytes of size `size`, relocations_complete)."""
    if len(image) < 52 or image[:4] != b'\x7fELF' or image[4] != 1 or image[5] != 1:
        raise IrxError('not a 32-bit little-endian ELF')
    (etype, machine, _version, entry, phoff, shoff, _flags, ehsize, phentsize, phnum, shentsize, shnum,
     _shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', image, 16)
    if machine != EM_MIPS or ehsize < 52:
        raise IrxError('not a MIPS ELF')

    def in_range(offset, size):
        return offset <= len(image) and size <= len(image) - offset

    programs = []
    min_vaddr, max_vaddr, has_load = 0xFFFFFFFF, 0, False
    if phnum and phentsize >= 32 and in_range(phoff, phentsize * phnum):
        for i in range(phnum):
            p = struct.unpack_from('<8I', image, phoff + i * phentsize)
            programs.append(p)
            ptype, _off, vaddr, _pa, _filesz, memsz = p[:6]
            if ptype == PT_LOAD and memsz:
                has_load = True
                min_vaddr = min(min_vaddr, vaddr)
                max_vaddr = max(max_vaddr, vaddr + memsz)
    sections = []
    if shnum and shentsize >= 40 and in_range(shoff, shentsize * shnum):
        for i in range(shnum):
            s = list(struct.unpack_from('<10I', image, shoff + i * shentsize))
            sections.append(s)
            if not has_load and (s[2] & SHF_ALLOC) and s[5]:
                min_vaddr = min(min_vaddr, s[3])
                max_vaddr = max(max_vaddr, s[3] + s[5])
    if min_vaddr == 0xFFFFFFFF:
        min_vaddr = 0
    span = align_up(max_vaddr - min_vaddr if max_vaddr > min_vaddr else 0x1000, 0x100)
    relocate = etype != ET_EXEC or max_vaddr > RAM_SIZE or (min_vaddr < MODULE_LOAD_BASE and min_vaddr != 0)
    if relocate:
        if base % 0x100:
            raise IrxError('base %#x is not 0x100-aligned' % base)
        delta = base - min_vaddr
    else:
        base, delta = min_vaddr, 0

    mem = bytearray(span)

    def write(addr, data):
        off = addr - base
        if off < 0 or off + len(data) > span:
            raise IrxError('segment outside the module at %#x' % addr)
        mem[off:off + len(data)] = data

    if has_load:
        for p in programs:
            ptype, off, vaddr, _pa, filesz, memsz = p[:6]
            if ptype != PT_LOAD or not memsz:
                continue
            if not in_range(off, filesz) or memsz < filesz:
                raise IrxError('bad PT_LOAD')
            write(vaddr + delta, image[off:off + filesz])
            write(vaddr + delta + filesz, bytes(memsz - filesz))
    else:
        cursor = base
        for s in sections:
            if not (s[2] & SHF_ALLOC) or not s[5]:
                continue
            if s[3]:
                dest = s[3] + delta
            else:
                cursor = align_up(cursor, max(s[8], 4))
                dest = cursor
                s[3] = dest - delta
                cursor += s[5]
            if s[1] == SHT_NOBITS:
                write(dest, bytes(s[5]))
            else:
                if not in_range(s[4], s[5]):
                    raise IrxError('bad section')
                write(dest, image[s[4]:s[4] + s[5]])

    def read32(addr):
        return struct.unpack_from('<I', mem, addr - base)[0]

    def write32(addr, value):
        struct.pack_into('<I', mem, addr - base, value & 0xFFFFFFFF)

    def s16(v):
        v &= 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    iop_rel = etype in (ET_SCE_IOPRELEXEC, ET_SCE_IOPRELEXEC2)
    complete = True
    hi16 = []
    for relsec in sections:
        if relsec[1] not in (SHT_REL, SHT_RELA) or relsec[7] >= len(sections):
            continue
        target_base = sections[relsec[7]][3] + delta
        symbols = []
        if relsec[6] < len(sections):
            sym = sections[relsec[6]]
            if sym[1] == SHT_SYMTAB and sym[9] >= 16 and in_range(sym[4], sym[5]):
                symbols = [struct.unpack_from('<IIIBBH', image, sym[4] + i * sym[9]) for i in range(sym[5] // sym[9])]
        rela = relsec[1] == SHT_RELA
        entsize = max(relsec[9], 12 if rela else 8)
        if not in_range(relsec[4], relsec[5]):
            continue
        for off in range(0, relsec[5] - entsize + 1, entsize):
            if rela:
                r_off, r_info, addend_x = struct.unpack_from('<IIi', image, relsec[4] + off)
            else:
                r_off, r_info = struct.unpack_from('<II', image, relsec[4] + off)
                addend_x = 0
            rtype, symidx = r_info & 0xFF, r_info >> 8
            value = base if iop_rel else 0
            if symidx < len(symbols) and (not iop_rel or symidx != 0):
                value = symbols[symidx][1]
                if symbols[symidx][5] != 0:
                    value += delta
            place = (base + r_off) if iop_rel else (target_base + r_off)
            if place + 3 >= RAM_SIZE or place < base or place + 4 > base + span:
                complete = False
                continue
            word = read32(place)
            addend = addend_x if rela else struct.unpack('<i', struct.pack('<I', word))[0]
            if rtype == 0:
                pass
            elif rtype in (2, 3):
                write32(place, addend + value)
            elif rtype == 4:
                target = ((word & 0x03FFFFFF) << 2) + value
                write32(place, (word & 0xFC000000) | ((target >> 2) & 0x03FFFFFF))
            elif rtype == 5:
                hi16.append((place, value, symidx))
            elif rtype == 6:
                lo = s16(word)
                keep = []
                for (haddr, hval, hsym) in hi16:
                    if hsym != symidx:
                        keep.append((haddr, hval, hsym))
                        continue
                    hiword = read32(haddr)
                    full = (s16(hiword) << 16) + lo + hval
                    write32(haddr, (hiword & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF))
                hi16 = keep
                write32(place, (word & 0xFFFF0000) | ((lo + value) & 0xFFFF))
            elif rtype == 1:
                write32(place, (word & 0xFFFF0000) | ((addend + value) & 0xFFFF))
            else:
                complete = False

    entry_out = (entry + delta) & 0xFFFFFFFF
    gp = 0
    for p in programs:
        if p[0] == PT_SCE_IOPMOD and p[4] >= 12 and in_range(p[1], 12):
            e, g = struct.unpack_from('<II', image, p[1] + 4)
            entry_out = (e + delta) & 0xFFFFFFFF
            gp = (g + delta) & 0xFFFFFFFF if g else 0
            break
    if not gp:
        for p in programs:
            if p[0] == PT_MIPS_REGINFO and p[4] >= 24 and in_range(p[1], 24):
                g = struct.unpack_from('<I', image, p[1] + 20)[0]
                gp = (g + delta) & 0xFFFFFFFF if g else 0
                break
    if not gp:
        for s in sections:
            if s[1] == SHT_MIPS_REGINFO and s[5] >= 24 and in_range(s[4], 24):
                g = struct.unpack_from('<I', image, s[4] + 20)[0]
                gp = (g + delta) & 0xFFFFFFFF if g else 0
                break
    return {'base': base, 'size': span, 'entry': entry_out, 'gp': gp, 'image': bytes(mem),
            'relocations_complete': complete}


def write_dump(irx_path, base, name, out_dir):
    """Writes <out>/<name>_<base>.bin and .json; returns the .json path."""
    with open(irx_path, 'rb') as f:
        data = f.read()
    m = load(data, base)
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, '%s_%06x' % (name, m['base']))
    with open(stem + '.bin', 'wb') as f:
        f.write(m['image'])
    with open(stem + '.json', 'w') as f:
        f.write('{"name": "%s", "base": %u, "size": %u, "entry": %u, "gp": %u, "hash": "%016x"}\n'
                % (name, m['base'], m['size'], m['entry'], m['gp'], fnv1a64(data)))
    if not m['relocations_complete']:
        print('%s: one or more relocations were unsupported' % name, file=sys.stderr)
    return stem + '.json'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('irx')
    ap.add_argument('--base', required=True, type=lambda s: int(s, 0))
    ap.add_argument('--name', help='module name as the IOP saw its path (default: the file name)')
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    try:
        print(write_dump(a.irx, a.base, a.name or os.path.basename(a.irx), a.out))
    except (OSError, IrxError) as e:
        sys.exit('irx_load: %s: %s' % (a.irx, e))


if __name__ == '__main__':
    main()
