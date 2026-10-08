"""Direct-jal targets must decode as code up to their first control transfer.

    python tools/test_compile_overlays_jal_targets.py
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays  # noqa: E402

LOAD = 0x80100000
JAL_TARGET = 0x0C000000 | ((LOAD + 0x20) & 0x0FFFFFFF) >> 2
JR_RA, NOP = 0x03E00008, 0x00000000
ADDIU_SP = 0x27BDFFE8  # addiu sp, sp, -24


def image(target_words):
    words = [JAL_TARGET, NOP, JR_RA, NOP, NOP, NOP, NOP, NOP] + target_words
    return struct.pack(f'<{len(words)}I', *words)


def direct_jals(data):
    walk = compile_overlays._walk_overlay_function(data, LOAD, len(data), LOAD, LOAD + 0x20)
    return walk['direct_jals']


class JalTargetTest(unittest.TestCase):
    def test_code_target_is_kept(self):
        data = image([ADDIU_SP, NOP, JR_RA, NOP])
        self.assertEqual(direct_jals(data), {LOAD + 0x20})

    def test_target_whose_second_word_is_data_is_dropped(self):
        # WE2002 ENDING.BIN: first word 0x2D010709 decodes (sltiu), the next (op 0x1E) does not
        data = image([0x2D010709, 0x7A80180B, JR_RA, NOP])
        self.assertEqual(direct_jals(data), set())


if __name__ == '__main__':
    unittest.main()
