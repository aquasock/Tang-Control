#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import sys
import unittest


sys.dont_write_bytecode = True
REPO_DIR = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "tangctl", REPO_DIR / "scripts" / "tangctl.py"
)
TANGCTL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TANGCTL)


class TangctlTimeoutTest(unittest.TestCase):
    def test_crc_timeout_scales_after_thirty_second_floor(self):
        mib = 1024 * 1024
        self.assertEqual(TANGCTL.crc_verify_timeout(0), 30)
        self.assertEqual(TANGCTL.crc_verify_timeout(20 * mib), 30)
        self.assertEqual(TANGCTL.crc_verify_timeout(21 * mib), 31)
        self.assertEqual(TANGCTL.crc_verify_timeout(42 * mib), 52)
        self.assertEqual(TANGCTL.crc_verify_timeout(42 * mib + 1), 53)


def synthetic_image(body_length=0x2000):
    """A minimal BL616 application image with a valid boot header."""
    import zlib
    header = bytearray(0x100)
    header[0:4] = b"BFNP"
    header[8:12] = b"FCFG"
    header[0x64:0x68] = b"PCFG"
    header[0x84:0x88] = body_length.to_bytes(4, "little")
    header[0xFC:0x100] = zlib.crc32(bytes(header[:0xFC])).to_bytes(4, "little")
    image = bytearray(0x1000 + body_length)
    image[:0x100] = header
    for index in range(0x1000, len(image)):
        image[index] = index * 7 & 0xFF
    return bytes(image)


class TangctlFirmwareImageTest(unittest.TestCase):
    def test_valid_image_reports_sha256(self):
        import hashlib
        image = synthetic_image()
        self.assertEqual(TANGCTL.check_boot_image(image), hashlib.sha256(image).hexdigest())

    def test_rejects_invalid_images(self):
        image = synthetic_image()
        cases = {
            "not a BL616 boot image": b"XFNP" + image[4:],
            "boot header CRC mismatch": image[:0x10] + b"\x01" + image[0x11:],
            "file size differs from boot header": image + b"\xff",
        }
        for message, data in cases.items():
            with self.subTest(message=message):
                with self.assertRaisesRegex(RuntimeError, message):
                    TANGCTL.check_boot_image(data)
        with self.assertRaisesRegex(RuntimeError, "image length out of range"):
            TANGCTL.check_boot_image(synthetic_image(0x80000))


if __name__ == "__main__":
    unittest.main()
