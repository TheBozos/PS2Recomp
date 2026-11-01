#!/usr/bin/env python3
"""Reads a PS2 disc image (ISO9660) without any outside tool: a plain .iso (2048-byte sectors), a raw .bin (2352 bytes
a sector, MODE1 or MODE2 form 1, with or without its .cue) or the .cue itself. Lists, reads and extracts the files
(names without their ";1" version, as bsdtar extracts them), and reads SYSTEM.CNF's boot ELF.

  tools/ps2_disc.py list <image>
  tools/ps2_disc.py boot <image>                prints the BOOT2 executable's name (SCUS_971.01, ...)
  tools/ps2_disc.py extract <image> <out dir>

Standard library only (Python 3.8+), so a bundled embeddable Python runs it.
"""
import os
import re
import struct
import sys


class DiscError(Exception):
    pass


SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


class Image:
    """Logical 2048-byte sectors of an image file: `sector_size` bytes a sector in the file, user data at
    `data_offset` in each."""

    def __init__(self, path):
        self.path = path
        if path.lower().endswith('.cue'):
            path, mode = self._parse_cue(path)
        else:
            mode = None
        try:
            self.f = open(path, 'rb')
        except OSError as e:
            raise DiscError('cannot open %s: %s' % (path, e.strerror))
        self.size = os.fstat(self.f.fileno()).st_size
        self.sector_size, self.data_offset = self._layout(mode)

    @staticmethod
    def _parse_cue(cue):
        base = os.path.dirname(os.path.abspath(cue))
        try:
            text = open(cue, 'r', errors='replace').read()
        except OSError as e:
            raise DiscError('cannot read %s: %s' % (cue, e.strerror))
        m = re.search(r'^\s*FILE\s+"([^"]+)"', text, re.M) or re.search(r'^\s*FILE\s+(\S+)', text, re.M)
        if not m:
            raise DiscError('%s names no FILE' % cue)
        t = re.search(r'^\s*TRACK\s+\d+\s+(\S+)', text, re.M)
        return os.path.join(base, m.group(1)), (t.group(1).upper() if t else None)

    def _layout(self, mode):
        known = {'MODE1/2048': (2048, 0), 'MODE1/2352': (2352, 16), 'MODE2/2352': (2352, 24),
                 'MODE2/2336': (2336, 8)}
        if mode in known:
            return known[mode]
        # No cue: a raw image starts each sector with the sync pattern; its mode byte says where the data is.
        self.f.seek(16 * 2352)
        raw = self.f.read(16)
        if self.size % 2352 == 0 and raw[:12] == SYNC:
            return (2352, 16) if raw[15] == 1 else (2352, 24)
        return 2048, 0

    def read_sectors(self, lba, count):
        if self.sector_size == 2048:
            self.f.seek(lba * 2048)
            data = self.f.read(count * 2048)
        else:
            parts = []
            for i in range(count):
                self.f.seek((lba + i) * self.sector_size + self.data_offset)
                parts.append(self.f.read(2048))
            data = b''.join(parts)
        if len(data) != count * 2048:
            raise DiscError('the image ends at sector %d (truncated download or copy?)' % (lba + len(data) // 2048))
        return data

    def read_extent(self, lba, size, chunk=8 << 20):
        """Yields the extent's bytes in chunks."""
        left = size
        while left > 0:
            sectors = min((left + 2047) // 2048, chunk // 2048)
            data = self.read_sectors(lba, sectors)
            yield data[:min(left, len(data))]
            lba += sectors
            left -= len(data)

    def close(self):
        self.f.close()


class Entry:
    __slots__ = ('path', 'lba', 'size', 'is_dir', 'mtime')

    def __init__(self, path, lba, size, is_dir, mtime):
        self.path, self.lba, self.size, self.is_dir, self.mtime = path, lba, size, is_dir, mtime


def _record_time(rec):
    y, mo, d, h, mi, s, tz = struct.unpack_from('<6Bb', rec, 18)
    try:
        import calendar
        return calendar.timegm((1900 + y, mo, d, h, mi, s, 0, 0, 0)) - tz * 15 * 60
    except (ValueError, OverflowError):
        return None


class Disc:
    def __init__(self, path):
        self.image = Image(path)
        pvd = self.image.read_sectors(16, 1)
        if pvd[1:6] != b'CD001' or pvd[0] != 1:
            self.image.close()
            raise DiscError('%s is not an ISO9660 disc image (no primary volume descriptor)' % path)
        self.volume_id = pvd[40:72].decode('latin-1').strip()
        root = pvd[156:156 + 34]
        self.root_lba, self.root_size = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
        self._entries = None

    def close(self):
        self.image.close()

    def _walk(self, lba, size, prefix, seen):
        if lba in seen:
            return
        seen.add(lba)
        data = self.image.read_sectors(lba, (size + 2047) // 2048)
        pos = 0
        subdirs = []
        while pos < size:
            length = data[pos]
            if length == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + length]
            pos += length
            name_len = rec[32]
            name = rec[33:33 + name_len]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('latin-1')
            flags = rec[25]
            if flags & 0x80:
                raise DiscError('multi-extent file %s is not supported' % name)
            if ';' in name:
                name = name.split(';')[0]
            if name.endswith('.') and not flags & 2:
                name = name[:-1]
            e = Entry(prefix + name, struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0],
                      bool(flags & 2), _record_time(rec))
            yield e
            if e.is_dir:
                subdirs.append(e)
        for d in subdirs:
            yield from self._walk(d.lba, d.size, d.path + '/', seen)

    def entries(self):
        if self._entries is None:
            self._entries = list(self._walk(self.root_lba, self.root_size, '', set()))
        return self._entries

    def find(self, path):
        want = path.replace('\\', '/').strip('/').upper()
        for e in self.entries():
            if e.path.upper() == want:
                return e
        return None

    def read(self, path):
        e = self.find(path)
        if e is None or e.is_dir:
            raise DiscError('%s is not on the disc' % path)
        return b''.join(self.image.read_extent(e.lba, e.size))

    def boot_elf(self):
        """The executable SYSTEM.CNF's BOOT2 line names (cdrom0:\\SCUS_971.01;1 -> SCUS_971.01), or None."""
        e = self.find('SYSTEM.CNF')
        if e is None:
            return None
        text = self.read('SYSTEM.CNF').decode('latin-1')
        m = re.search(r'BOOT2\s*=\s*cdrom0:\\?([^;\r\n]+)', text, re.I)
        return m.group(1).strip().lstrip('\\') if m else None

    def extract(self, out_dir, progress=None):
        """Extracts every file under out_dir; progress(done_bytes, total_bytes, path) after each chunk."""
        files = [e for e in self.entries() if not e.is_dir]
        total = sum(e.size for e in files)
        done = 0
        for e in self.entries():
            if e.is_dir:
                os.makedirs(os.path.join(out_dir, e.path), exist_ok=True)
        for e in files:
            dest = os.path.join(out_dir, e.path)
            os.makedirs(os.path.dirname(dest) or out_dir, exist_ok=True)
            tmp = dest + '.part'
            with open(tmp, 'wb') as f:
                for chunk in self.image.read_extent(e.lba, e.size):
                    f.write(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total, e.path)
            os.replace(tmp, dest)
            if e.mtime is not None:
                try:
                    os.utime(dest, (e.mtime, e.mtime))
                except OSError:
                    pass
        return len(files), total


def main(argv):
    if len(argv) < 3 or argv[1] not in ('list', 'boot', 'extract'):
        sys.exit(__doc__)
    try:
        disc = Disc(argv[2])
        if argv[1] == 'list':
            for e in disc.entries():
                print('%s%s %d' % (e.path, '/' if e.is_dir else '', e.size))
        elif argv[1] == 'boot':
            print(disc.boot_elf() or '')
        else:
            if len(argv) < 4:
                sys.exit(__doc__)
            n, total = disc.extract(argv[3])
            print('%d files, %d bytes -> %s' % (n, total, argv[3]))
    except DiscError as e:
        sys.exit('ps2_disc: %s' % e)


if __name__ == '__main__':
    main(sys.argv)
