import importlib.util
from pathlib import Path
import unittest


MODULE = Path(__file__).parents[1] / "tools/audit_origo_elf.py"
SPEC = importlib.util.spec_from_file_location("audit_origo_elf", MODULE)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class LinkedMapTextTests(unittest.TestCase):
    """linked_map_text() has to drop exactly the "Discarded input sections"
    block: keep it and unused-but-mentioned archives read as linked; drop too
    much (as the split-on-that-header version this replaces did) and the real
    "Linker script and memory map" listing goes unchecked instead."""

    MAP = (
        "Archive member included to satisfy reference by file (symbol)\n"
        "libmain.a(seedtool_app.c.obj)\n"
        "Discarded input sections\n"
        " .text          0x0000000000000000        0x0 libesp_wifi.a(esp_wifi.c.obj)\n"
        "Memory Configuration\n"
        "Name             Origin             Length             Attributes\n"
        "Linker script and memory map\n"
        "LOAD libmain.a\n"
        "LOAD libnvs_flash.a\n"
    )

    def test_excludes_the_discarded_block(self):
        linked = audit.linked_map_text(self.MAP)
        self.assertNotIn("libesp_wifi.a", linked)

    def test_keeps_the_linker_memory_map_that_follows_it(self):
        # Regression: a version that kept only the text before "Discarded
        # input sections" dropped "Linker script and memory map" along with
        # it, so a forbidden archive linked in for real - present only in
        # that section, as libnvs_flash.a is here - went unchecked.
        linked = audit.linked_map_text(self.MAP)
        self.assertIn("libnvs_flash.a", linked)
        self.assertIn("libmain.a", linked)

    def test_returns_the_whole_text_when_the_headers_are_missing(self):
        self.assertEqual(audit.linked_map_text("no headers here"), "no headers here")


if __name__ == "__main__":
    unittest.main()
