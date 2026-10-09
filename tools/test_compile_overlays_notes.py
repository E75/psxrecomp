"""Overlay function notes land above the named function of the right image only.

    python tools/test_compile_overlays_notes.py
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_overlays  # noqa: E402

SRC = ('void ov_X_func_80110000(CPUState* cpu);\n'
       '\nvoid ov_X_func_80110000(CPUState* cpu)\n{\n}\n'
       '\nvoid ov_X_func_80110040(CPUState* cpu)\n{\n}\n')
SYMBOLS = {0x80110000: 'ov_X_func_80110000', 0x80110040: 'ov_X_func_80110040'}


def notes(text: str) -> dict:
    with tempfile.NamedTemporaryFile('w', suffix='.csv', delete=False, encoding='utf-8') as f:
        f.write(text)
    return compile_overlays.load_overlay_notes(f.name)


class OverlayNotesTest(unittest.TestCase):
    def test_note_goes_above_the_definition_only(self):
        table = notes('# names\n80110000,0x80110000,Menu_Draw (traced): draws */ the menu\n')
        out = compile_overlays.annotate_overlay_functions(SRC, SYMBOLS, table, 0x00110000, 0x1234)
        self.assertIn('\n/* [NOTE] Menu_Draw (traced): draws * / the menu */\nvoid ov_X_func_80110000(CPUState* cpu)\n{',
                      out)
        self.assertEqual(out.count('[NOTE]'), 1)           # prototype and other function untouched
        self.assertTrue(out.startswith('void ov_X_func_80110000(CPUState* cpu);\n'))

    def test_crc_selects_one_image_at_a_shared_load_address(self):
        table = notes('80110000/0000ABCD,0x80110040,Select_Update\n')
        other = compile_overlays.annotate_overlay_functions(SRC, SYMBOLS, table, 0x00110000, 0x1234)
        self.assertNotIn('[NOTE]', other)
        same = compile_overlays.annotate_overlay_functions(SRC, SYMBOLS, table, 0x00110000, 0xABCD)
        self.assertIn('/* [NOTE] Select_Update */\nvoid ov_X_func_80110040', same)


if __name__ == '__main__':
    unittest.main()
