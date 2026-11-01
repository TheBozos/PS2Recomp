"""The mesh pack (PS2XMSH1): a game's 3D objects converted offline, loaded by the fork's runtime
(runtime/native/native_mesh_pack.h, ps2x::native::packs::MeshPack). docs/native_packs.md.

A port's converter makes one MeshFile per game file (its name as the port's runtime will name it when the game loads
that file, and the image as the game has it in memory, for the size and CRC32 check), adds its objects by their offset
in that image, and writes the pack:

  pack = MeshPackWriter()
  f = pack.add_file('JUNKYARD.NGP', image)
  o = f.add_object(offset, kind=0, attributes=struct.pack(...))
  o.positions = [(x, y, z, w), ...]
  o.add_section(b'AUX ', data)                                   # port-defined blocks, by tag
  o.add_strip(prim, gif_flags, material, slot, [(position, nx, ny, nz, flags, (r, g, b, a), s, t), ...])
  f.add_register_block(offset, [(gs_register, value), ...])
  pack.write(path)

The format, all little-endian:
  "PS2XMSH1", u32 file count, then per file:
    u16 name length, name, u32 image size, u32 image CRC32 (zlib.crc32), u32 object count, then per object (sorted
    by offset):
      u32 offset, u32 kind (the port's), u32 attribute size, the attributes (the port's fixed fields),
      u32 position count, per position f32 x, y, z, w,
      u32 section count, per section u32 tag (four characters), u32 size, its bytes (the port's),
      u32 strip count, per strip a 16-byte header: u16 prim (GIF PRIM, 11 bits), u16 gif flags (bit 0 PRE, bit 1
        EOP), u32 material (the port's word: TMB the GIF tag's word 3), i32 texture slot (-1: keep the current one),
        u32 vertex count; then per vertex 16 bytes: u16 position index, u8 normal x, y, z (raw), u8 flags (1 inline
        point, 2 ADC, 4 a corner expanded from its point), u8 r, g, b, a, s16 s, t (raw), u16 extra (the port's).
    u32 register block count, per block (sorted by offset): u32 offset in the image, u32 write count, per write u32 GS
      register address, u64 value.
"""
import struct
import zlib

MAGIC = b'PS2XMSH1'
VERTEX_INLINE, VERTEX_ADC, VERTEX_CORNER = 1, 2, 4


def fourcc(tag):
    """A section tag (b'AUX ' or 'AUX ') as the runtime's ps2x::native::packs::fourcc gives it."""
    if isinstance(tag, str):
        tag = tag.encode('ascii')
    if len(tag) != 4:
        raise ValueError(f'section tag {tag!r}: four characters')
    return struct.unpack('<I', tag)[0]


class Strip:
    """One triangle strip: GIF PRIM, GIF flags, the port's material word, texture slot (-1 keeps the current one) and
    vertices (position index, nx, ny, nz, flags, (r, g, b, a), s, t[, extra])."""

    def __init__(self, prim, gif_flags, material, slot, vertices):
        self.prim, self.gif_flags, self.material, self.slot = prim, gif_flags, material, slot
        self.vertices = list(vertices)

    def encode(self):
        out = [struct.pack('<HHIiI', self.prim, self.gif_flags, self.material, self.slot, len(self.vertices))]
        for v in self.vertices:
            extra = v[8] if len(v) > 8 else 0
            out.append(struct.pack('<H4B4B2hH', v[0], v[1], v[2], v[3], v[4], *v[5], v[6], v[7], extra))
        return b''.join(out)


class MeshObject:
    def __init__(self, offset, kind=0, attributes=b''):
        self.offset, self.kind, self.attributes = offset, kind, bytes(attributes)
        self.positions = []  # (x, y, z, w)
        self.sections = []   # (tag, bytes)
        self.strips = []     # Strip

    def add_section(self, tag, data):
        fourcc(tag)
        self.sections.append((tag.encode('ascii') if isinstance(tag, str) else bytes(tag), bytes(data)))

    def add_strip(self, prim, gif_flags, material, slot, vertices):
        strip = Strip(prim, gif_flags, material, slot, vertices)
        self.strips.append(strip)
        return strip

    def encode(self):
        out = [struct.pack('<III', self.offset, self.kind, len(self.attributes)), self.attributes,
               struct.pack('<I', len(self.positions))]
        out.extend(struct.pack('<4f', *p) for p in self.positions)
        out.append(struct.pack('<I', len(self.sections)))
        for tag, data in self.sections:
            out.append(struct.pack('<II', fourcc(tag), len(data)) + data)
        out.append(struct.pack('<I', len(self.strips)))
        out.extend(s.encode() for s in self.strips)
        return b''.join(out)


class MeshFile:
    def __init__(self, name, image=None, size=None, crc=None):
        """name: as the port's runtime names the loaded file; image: the bytes the game has in memory (or size and crc)."""
        self.name = name
        self.size = len(image) if image is not None else size
        self.crc = (zlib.crc32(image) & 0xffffffff) if image is not None else crc
        self.objects = []
        self.register_blocks = []  # (offset, [(register, value)])

    def add_object(self, offset, kind=0, attributes=b''):
        obj = MeshObject(offset, kind, attributes)
        self.objects.append(obj)
        return obj

    def add_register_block(self, offset, writes):
        self.register_blocks.append((offset, list(writes)))

    def encode(self):
        name = self.name.encode()
        out = [struct.pack('<H', len(name)) + name, struct.pack('<III', self.size, self.crc, len(self.objects))]
        offsets = [o.offset for o in self.objects]
        if len(set(offsets)) != len(offsets):
            raise ValueError(f'{self.name}: two objects at one offset')
        out.extend(o.encode() for o in sorted(self.objects, key=lambda o: o.offset))
        blocks = sorted(self.register_blocks, key=lambda b: b[0])
        out.append(struct.pack('<I', len(blocks)))
        for offset, writes in blocks:
            out.append(struct.pack('<II', offset, len(writes)))
            out.extend(struct.pack('<IQ', a, v) for a, v in writes)
        return b''.join(out)


class MeshPackWriter:
    def __init__(self):
        self.files = []

    def add_file(self, name, image=None, size=None, crc=None):
        f = MeshFile(name, image, size, crc)
        self.files.append(f)
        return f

    def write(self, path):
        with open(path, 'wb') as out:
            out.write(MAGIC + struct.pack('<I', len(self.files)))
            for f in self.files:
                out.write(f.encode())

    def object_count(self):
        return sum(len(f.objects) for f in self.files)


def read_pack(path):
    """The pack back as [MeshFile] (objects with their positions, sections and strips): for checks and dumps."""
    data = open(path, 'rb').read()
    if data[:8] != MAGIC:
        raise ValueError(f'{path}: not a {MAGIC.decode()} pack')
    at = 8
    (count,) = struct.unpack_from('<I', data, at)
    at += 4
    files = []
    for _ in range(count):
        (n,) = struct.unpack_from('<H', data, at)
        name = data[at + 2:at + 2 + n].decode()
        at += 2 + n
        size, crc, objects = struct.unpack_from('<III', data, at)
        at += 12
        f = MeshFile(name, size=size, crc=crc)
        for _ in range(objects):
            offset, kind, asize = struct.unpack_from('<III', data, at)
            at += 12
            o = f.add_object(offset, kind, data[at:at + asize])
            at += asize
            (positions,) = struct.unpack_from('<I', data, at)
            at += 4
            o.positions = [struct.unpack_from('<4f', data, at + 16 * i) for i in range(positions)]
            at += 16 * positions
            (sections,) = struct.unpack_from('<I', data, at)
            at += 4
            for _ in range(sections):
                tag, ssize = struct.unpack_from('<II', data, at)
                o.sections.append((struct.pack('<I', tag), data[at + 8:at + 8 + ssize]))
                at += 8 + ssize
            (strips,) = struct.unpack_from('<I', data, at)
            at += 4
            for _ in range(strips):
                prim, gif_flags, material, slot, nv = struct.unpack_from('<HHIiI', data, at)
                at += 16
                vertices = []
                for i in range(nv):
                    v = struct.unpack_from('<H4B4B2hH', data, at + 16 * i)
                    vertices.append((v[0], v[1], v[2], v[3], v[4], tuple(v[5:9]), v[9], v[10], v[11]))
                at += 16 * nv
                o.strips.append(Strip(prim, gif_flags, material, slot, vertices))
        (blocks,) = struct.unpack_from('<I', data, at)
        at += 4
        for _ in range(blocks):
            offset, writes = struct.unpack_from('<II', data, at)
            at += 8
            f.add_register_block(offset, [struct.unpack_from('<IQ', data, at + 12 * i) for i in range(writes)])
            at += 12 * writes
        files.append(f)
    return files
