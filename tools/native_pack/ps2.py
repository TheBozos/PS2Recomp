"""PS2 data helpers for a converter (docs/native_packs.md): the VIF codes of a packet, the DMA chain of a prebuilt packet
in a memory image linked at a fixed address, the GS A+D register blocks of an image, and an ELF's bytes at an address.
"""
import struct

UNPACK_FORMATS = {0: 'S-32', 1: 'S-16', 2: 'S-8', 4: 'V2-32', 5: 'V2-16', 6: 'V2-8', 8: 'V3-32', 9: 'V3-16',
                  10: 'V3-8', 12: 'V4-32', 13: 'V4-16', 14: 'V4-8', 15: 'V4-5'}


def u16(data, at):
    return struct.unpack_from('<H', data, at)[0]


def u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


def unpack_size(fmt, count):
    """The bytes an UNPACK of `count` vectors of format `fmt` (its cmd's low 4 bits) takes, padded to a word."""
    vn, vl = (fmt >> 2) & 3, fmt & 3
    return ((32, 16, 8, 5)[vl] * (vn + 1) * count + 31) // 32 * 4


def vif_codes(data, pos, end):
    """The VIF codes in data[pos:end]: (offset, cmd, num, imm, payload offset). cmd without its interrupt bit; num the
    UNPACK's vector count, DIRECT's quadword count, MPG's instruction count."""
    while pos + 4 <= end:
        code = u32(data, pos)
        cmd, num, imm = (code >> 24) & 0x7f, (code >> 16) & 0xff, code & 0xffff
        at = pos
        pos += 4
        if cmd >= 0x60:  # UNPACK
            count = num or 256
            yield at, cmd, count, imm, pos
            pos += unpack_size(cmd & 0xf, count)
        elif cmd in (0x50, 0x51):  # DIRECT, DIRECTHL
            yield at, cmd, imm or 65536, imm, pos
            pos += (imm or 65536) * 16
        elif cmd == 0x4a:  # MPG
            yield at, cmd, num or 256, imm, pos
            pos += (num or 256) * 8
        elif cmd in (0x30, 0x31):  # STROW, STCOL
            yield at, cmd, 0, imm, pos
            pos += 16
        elif cmd == 0x20:  # STMASK
            yield at, cmd, 0, imm, pos
            pos += 4
        else:
            yield at, cmd, num, imm, pos


def dma_chain(image, start, link, limit=256):
    """A prebuilt DMA chain in a memory image linked at `link` (an address A is image offset A - link), from the tag at
    `start`: [(VIF bytes, their offset in the image)], each tag's upper half (its two VIF codes) first. REF / REFS: data
    elsewhere, the next tag after this one; CNT: data after the tag, the next tag after it; NEXT: data after the tag,
    the next tag at its address; RET / END: data after the tag, the end. (CALL is not followed: ValueError.)"""
    segments = []
    at = start
    for _ in range(limit):
        lo, addr = u32(image, at), u32(image, at + 4)
        qwc, tag = lo & 0xffff, (lo >> 28) & 7
        segments.append((image[at + 8:at + 16], at + 8))
        if tag in (3, 4):  # REF / REFS
            segments.append((image[addr - link:addr - link + qwc * 16], addr - link))
            at += 16
        elif tag in (1, 6, 7):  # CNT / RET / END
            segments.append((image[at + 16:at + 16 + qwc * 16], at + 16))
            if tag != 1:
                return segments
            at += 16 + qwc * 16
        elif tag == 2:  # NEXT
            segments.append((image[at + 16:at + 16 + qwc * 16], at + 16))
            at = addr - link
        else:
            raise ValueError(f'DMA tag {tag} at {at:#x}')
    raise ValueError(f'DMA chain from {start:#x} does not end')


def chain_stream(image, start, link):
    """The VIF stream a chain sends, with where each piece of it is in the image: (stream, [(stream offset, image
    offset, length)])."""
    pieces, at = [], 0
    segments = dma_chain(image, start, link)
    for data, offset in segments:
        pieces.append((at, offset, len(data)))
        at += len(data)
    return b''.join(data for data, _ in segments), pieces


def image_offset(pieces, at, length):
    """The image offset of stream bytes [at, at + length) when one piece holds them all, else None."""
    for start, offset, size in pieces:
        if start <= at and at + length <= start + size:
            return offset + at - start
    return None


def find_gs_register_blocks(image, keep=None, max_register=0x63):
    """The GS register blocks in an image, as a game keeps them to send with its draws: in a quadword, FLUSH and
    DIRECT / DIRECTHL n in its upper words (its lower two 0), then a PACKED GIF tag (NREG 1, A+D) of NLOOP <= n - 1
    writes to GS registers (each address <= max_register). Returns [(offset, [(address, value)])], the writes those
    whose address is in `keep` (all when None)."""
    found = []
    at = image.find(b'\x00\x00\x00\x11')
    while at >= 0:
        o = at - 8
        if o >= 0 and o % 16 == 0 and o + 32 <= len(image):
            w0, w1, _, code = struct.unpack_from('<4I', image, o)
            tag, regs = struct.unpack_from('<QQ', image, o + 16)
            nloop, flg, nreg = tag & 0x7fff, (tag >> 58) & 3, (tag >> 60) & 0xf
            n = code & 0xffff
            if (w0 == 0 and w1 == 0 and (code >> 24) & 0x7e == 0x50 and flg == 0 and nreg == 1 and regs & 0xf == 0xe
                    and 0 < nloop and nloop + 1 <= n and o + 32 + nloop * 16 <= len(image)):
                writes = []
                ok = True
                for q in range(nloop):
                    value, address = struct.unpack_from('<QQ', image, o + 32 + q * 16)
                    if address > max_register:
                        ok = False
                        break
                    if keep is None or address in keep:
                        writes.append((address, value))
                if ok:
                    found.append((o, writes))
        at = image.find(b'\x00\x00\x00\x11', at + 1)
    return found


def elf_bytes(path, address, size):
    """`size` bytes at an address of an ELF's loaded segments."""
    elf = open(path, 'rb').read()
    phoff, phentsize, phnum = u32(elf, 0x1c), u16(elf, 0x2a), u16(elf, 0x2c)
    for i in range(phnum):
        p = phoff + i * phentsize
        offset, vaddr, filesz = u32(elf, p + 4), u32(elf, p + 8), u32(elf, p + 16)
        if vaddr <= address and address + size <= vaddr + filesz:
            return elf[offset + address - vaddr:offset + address - vaddr + size]
    raise ValueError(f'{address:#x} not in the ELF')
