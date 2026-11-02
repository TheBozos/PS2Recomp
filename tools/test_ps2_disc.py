#!/usr/bin/env python3
"""Tests of tools/ps2_disc.py on synthetic ISO9660 images: python3 tools/test_ps2_disc.py"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps2_disc  # noqa: E402


def both_endian(fmt, value):
    return struct.pack('<' + fmt, value) + struct.pack('>' + fmt, value)


def record(name, lba, size, is_dir):
    raw = name if isinstance(name, bytes) else name.encode('ascii')
    body = (b'\x00' + both_endian('I', lba) + both_endian('I', size) + bytes([103, 1, 2, 3, 4, 5, 0])
            + bytes([2 if is_dir else 0, 0, 0]) + both_endian('H', 1) + bytes([len(raw)]) + raw)
    if len(body) % 2 == 0:
        body += b'\x00'
    return bytes([len(body) + 1]) + body


def directory(lba, parent, children):
    data = record(b'\x00', lba, 2048, True) + record(b'\x01', parent, 2048, True)
    for name, child_lba, size, is_dir in children:
        data += record(name, child_lba, size, is_dir)
    return data.ljust(2048, b'\x00')


def pvd(volume_sectors, root_lba):
    s = bytearray(2048)
    s[0:6] = b'\x01CD001'
    s[6] = 1
    s[40:72] = b'TEST'.ljust(32)
    s[80:88] = both_endian('I', volume_sectors)
    s[156:190] = record(b'\x00', root_lba, 2048, True)
    return bytes(s)


def volume(sectors, files, subdir_files):
    """A volume of `sectors` sectors: root (sector 18) with `files` and SUB/ (sector 19) with `subdir_files`,
    file data from sector 20. Sector numbers are the volume's own."""
    img = bytearray(sectors * 2048)
    img[16 * 2048:17 * 2048] = pvd(sectors, 18)
    img[17 * 2048:17 * 2048 + 7] = b'\xffCD001\x01'
    lba = 20
    entries = {}
    for name, data in list(files.items()) + list(subdir_files.items()):
        img[lba * 2048:lba * 2048 + len(data)] = data
        entries[name] = (lba, len(data))
        lba += (len(data) + 2047) // 2048 or 1
    root = [(n + ';1', *entries[n], False) for n in files] + [('SUB', 19, 2048, True)]
    sub = [(n + ';1', *entries[n], False) for n in subdir_files]
    img[18 * 2048:19 * 2048] = directory(18, 18, root)
    img[19 * 2048:20 * 2048] = directory(19, 18, sub)
    return bytes(img)


class DiscTest(unittest.TestCase):
    def image(self, data):
        f = tempfile.NamedTemporaryFile(suffix='.iso', delete=False)
        f.write(data)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_single_layer(self):
        path = self.image(volume(32, {'SYSTEM.CNF': b'BOOT2 = cdrom0:\\SLUS_000.01;1\r\n'}, {'A.BIN': b'a' * 3000}))
        disc = ps2_disc.Disc(path)
        self.addCleanup(disc.close)
        self.assertEqual(len(disc.volumes), 1)
        self.assertEqual(sorted(e.path for e in disc.entries()), ['SUB', 'SUB/A.BIN', 'SYSTEM.CNF'])
        self.assertEqual(disc.read('SUB/A.BIN'), b'a' * 3000)
        self.assertEqual(disc.boot_elf(), 'SLUS_000.01')

    def test_second_layer_files_after_the_first_layers(self):
        layer0 = volume(32, {'SYSTEM.CNF': b'BOOT2 = cdrom0:\\SLUS_000.01;1\r\n', 'SAME.BIN': b's' * 100},
                        {'A.BIN': b'a' * 3000})
        layer1 = volume(40, {'SAME.BIN': b'S' * 100}, {'B.BIN': b'b' * 5000, 'A.BIN': b'X' * 10})
        path = self.image(layer0 + layer1[16 * 2048:])
        disc = ps2_disc.Disc(path)
        self.addCleanup(disc.close)
        self.assertEqual(len(disc.volumes), 2)
        self.assertEqual([e.path for e in disc.entries()],
                         ['SYSTEM.CNF', 'SAME.BIN', 'SUB', 'SUB/A.BIN', 'SUB/B.BIN'])
        self.assertEqual(disc.read('SUB/B.BIN'), b'b' * 5000)
        self.assertEqual(disc.read('SUB/A.BIN'), b'a' * 3000)
        self.assertEqual(disc.read('SAME.BIN'), b's' * 100)
        with tempfile.TemporaryDirectory() as out:
            count, total = disc.extract(out)
            self.assertEqual((count, total), (4, 31 + 100 + 3000 + 5000))
            self.assertEqual(open(os.path.join(out, 'SUB', 'B.BIN'), 'rb').read(), b'b' * 5000)

    def test_padding_after_the_first_layer_is_not_a_second_volume(self):
        path = self.image(volume(32, {'SYSTEM.CNF': b'x'}, {}) + bytes(64 * 2048))
        disc = ps2_disc.Disc(path)
        self.addCleanup(disc.close)
        self.assertEqual(len(disc.volumes), 1)


if __name__ == '__main__':
    unittest.main()
