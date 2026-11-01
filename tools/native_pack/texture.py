"""The texture pack (PS2XTEX1): a game's textures and palettes converted offline, loaded by the fork's runtime
(runtime/native/native_texture_pack.h, ps2x::native::packs::TexturePack). docs/native_packs.md.

A port's converter lists, per game file (named as its runtime will look it up), the records in the order its runtime
key scheme counts them, each a PS2 raster (PSM, width, height, the texels in that format):

  pack = TexturePackWriter()
  records = pack.add_file('JUNKYARD.TEX')
  records.append(record(PSMT8, 128, 64, raw))                  # raw: the texels as the GS stores them (packed)
  pack.write(path)

The format, all little-endian: "PS2XTEX1", u32 file count, then per file u16 name length, name, u32 record count, then
per record u32 psm, u32 width, u32 height, u32 byte count, the texels unpacked: indexed formats (T8, T4, T8H, T4HL,
T4HH) one byte per texel (the index), CT32 four bytes, CT24 four bytes (alpha 0), CT16 / CT16S two bytes.
"""
import struct

MAGIC = b'PS2XTEX1'
PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH = (
    0x00, 0x01, 0x02, 0x0a, 0x13, 0x14, 0x1b, 0x24, 0x2c)
INDEXED = (PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH)


def bits(psm):
    """Bits per texel as the GS stores the format."""
    return {PSMCT32: 32, PSMCT24: 24, PSMCT16: 16, PSMCT16S: 16, PSMT8: 8, PSMT8H: 8}.get(psm, 4)


def raster_bytes(psm, width, height):
    return (width * height * bits(psm) + 7) // 8


def unpack_texels(psm, raw, width, height):
    """A raster as the GS stores it (4-bit texels low nibble first, CT24 three bytes) into the pack's texels."""
    count = width * height
    if psm in (PSMT8, PSMT8H):
        return bytes(raw[:count])
    if psm in (PSMT4, PSMT4HL, PSMT4HH):
        out = bytearray(count)
        for i in range(count):
            byte = raw[i >> 1]
            out[i] = (byte >> 4) if i & 1 else (byte & 0xf)
        return bytes(out)
    if psm == PSMCT32:
        return bytes(raw[:count * 4])
    if psm == PSMCT24:
        out = bytearray(count * 4)
        for i in range(count):
            out[i * 4:i * 4 + 3] = raw[i * 3:i * 3 + 3]
        return bytes(out)
    if psm in (PSMCT16, PSMCT16S):
        return bytes(raw[:count * 2])
    raise ValueError(f'psm {psm:#x}')


def record(psm, width, height, raw):
    """A record from a stored raster (shorter than the format's size: zeros after it)."""
    need = raster_bytes(psm, width, height)
    raw = bytes(raw)
    if len(raw) < need:
        raw = raw + bytes(need - len(raw))
    return psm, width, height, unpack_texels(psm, raw, width, height)


class TexturePackWriter:
    def __init__(self):
        self.files = []  # (name, [(psm, width, height, texels)])

    def add_file(self, name, records=None):
        """The file's record list (append (psm, width, height, texels) records to it)."""
        records = list(records or [])
        self.files.append((name, records))
        return records

    def write(self, path):
        with open(path, 'wb') as out:
            out.write(MAGIC + struct.pack('<I', len(self.files)))
            for name, records in self.files:
                encoded = name.encode()
                out.write(struct.pack('<H', len(encoded)) + encoded + struct.pack('<I', len(records)))
                for psm, width, height, texels in records:
                    out.write(struct.pack('<IIII', psm, width, height, len(texels)) + texels)

    def record_count(self):
        return sum(len(r) for _, r in self.files)


def read_pack(path):
    """The pack back as [(name, [(psm, width, height, texels)])]."""
    data = open(path, 'rb').read()
    if data[:8] != MAGIC:
        raise ValueError(f'{path}: not a {MAGIC.decode()} pack')
    (count,) = struct.unpack_from('<I', data, 8)
    at, files = 12, []
    for _ in range(count):
        (n,) = struct.unpack_from('<H', data, at)
        name = data[at + 2:at + 2 + n].decode()
        at += 2 + n
        (records,) = struct.unpack_from('<I', data, at)
        at += 4
        out = []
        for _ in range(records):
            psm, width, height, size = struct.unpack_from('<IIII', data, at)
            out.append((psm, width, height, data[at + 16:at + 16 + size]))
            at += 16 + size
        files.append((name, out))
    return files
