"""Tests of extract_rpi_fat.py on small synthetic images."""

import hashlib
import lzma
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import extract_rpi_fat  # noqa: E402
from prepare_vm import PrepareError  # noqa: E402


def mbr_with(entries: list) -> bytes:
    sector = bytearray(512)
    for index, (kind, first, count) in enumerate(entries):
        base = 446 + 16 * index
        sector[base + 4] = kind
        sector[base + 8: base + 12] = first.to_bytes(4, 'little')
        sector[base + 12: base + 16] = count.to_bytes(4, 'little')
    sector[510] = 0x55
    sector[511] = 0xAA
    return bytes(sector)


def fat_bytes(sectors: int, fill: int) -> bytes:
    body = bytearray([fill]) * (sectors * 512)
    body[510] = 0x55
    body[511] = 0xAA
    return bytes(body)


class ExtractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write_image(self, raw: bytes) -> str:
        path = os.path.join(self.directory.name, 'test-RPI.img.xz')
        with lzma.open(path, 'wb') as stream:
            stream.write(raw)
        return path

    def test_copies_the_first_fat_slice(self) -> None:
        fat = fat_bytes(8, 7)
        raw = mbr_with([(0xA5, 40, 4), (0x0C, 16, 8)]) + bytes(15 * 512) + fat + bytes(64 * 512)
        out = os.path.join(self.directory.name, 'fat.img')
        kind, first, count, sha = extract_rpi_fat.extract(self.write_image(raw), out)
        self.assertEqual((kind, first, count), (0x0C, 16 * 512, 8 * 512))
        with open(out, 'rb') as stream:
            self.assertEqual(stream.read(), fat)
        self.assertEqual(sha, hashlib.sha256(fat).hexdigest())

    def test_refuses_a_missing_signature(self) -> None:
        raw = bytearray(mbr_with([(0x0C, 1, 2)]) + fat_bytes(2, 1))
        raw[511] = 0
        with self.assertRaises(PrepareError):
            extract_rpi_fat.extract(self.write_image(bytes(raw)), os.path.join(self.directory.name, 'x'))

    def test_refuses_no_fat_slice(self) -> None:
        raw = mbr_with([(0xA5, 1, 2)]) + bytes(4 * 512)
        with self.assertRaises(PrepareError):
            extract_rpi_fat.extract(self.write_image(raw), os.path.join(self.directory.name, 'x'))

    def test_refuses_a_slice_past_the_end(self) -> None:
        raw = mbr_with([(0x0C, 1, 100)]) + fat_bytes(2, 1)
        out = os.path.join(self.directory.name, 'x')
        with self.assertRaises(PrepareError):
            extract_rpi_fat.extract(self.write_image(raw), out)
        self.assertFalse(os.path.exists(out))
        self.assertFalse(os.path.exists(out + '.part'))

    def test_refuses_a_slice_without_a_boot_sector(self) -> None:
        raw = mbr_with([(0x0C, 1, 2)]) + bytes(2 * 512)
        out = os.path.join(self.directory.name, 'x')
        with self.assertRaises(PrepareError):
            extract_rpi_fat.extract(self.write_image(raw), out)
        self.assertFalse(os.path.exists(out))

    def test_main_checks_the_checksum(self) -> None:
        image = self.write_image(mbr_with([(0x0C, 1, 2)]) + fat_bytes(2, 3))
        sums = os.path.join(self.directory.name, 'CHECKSUM')
        with open(sums, 'w', encoding='ascii') as stream:
            stream.write(f'SHA256 (test-RPI.img.xz) = {"0" * 64}\n')
        out = os.path.join(self.directory.name, 'fat.img')
        self.assertEqual(extract_rpi_fat.main(['--image', image, '--checksums', sums, '--out', out]), 1)
        self.assertFalse(os.path.exists(out))
        with open(image, 'rb') as stream:
            good = hashlib.sha256(stream.read()).hexdigest()
        with open(sums, 'w', encoding='ascii') as stream:
            stream.write(f'SHA256 (test-RPI.img.xz) = {good}\n')
        self.assertEqual(extract_rpi_fat.main(['--image', image, '--checksums', sums, '--out', out]), 0)
        self.assertTrue(os.path.exists(out))


if __name__ == '__main__':
    unittest.main()
