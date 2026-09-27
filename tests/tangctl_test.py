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


if __name__ == "__main__":
    unittest.main()
