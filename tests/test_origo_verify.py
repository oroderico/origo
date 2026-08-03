import importlib.util
from pathlib import Path
import unittest


MODULE = Path(__file__).parents[1] / "tools/origo_verify.py"
SPEC = importlib.util.spec_from_file_location("origo_verify", MODULE)
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)


class SeedToolVerifierTests(unittest.TestCase):
    MNEMONIC = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"

    def test_bip39_zero_vector(self):
        self.assertEqual(verify.mnemonic_from_entropy(bytes(16)), self.MNEMONIC)
        self.assertEqual(verify.mnemonic_entropy(self.MNEMONIC), bytes(16))

    def test_bad_checksum_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "checksum"):
            verify.mnemonic_entropy(self.MNEMONIC.replace("about", "abandon"))

    def test_krux_compatible_d6_transcript_vector(self):
        entries = list("123456" * 8 + "12")
        text = verify.transcript("d6", entries, 50)
        self.assertEqual(text, "12345612345612345612345612345612345612345612345612")
        self.assertEqual(
            verify.hashlib.sha256(text.encode()).hexdigest(),
            "ee72ae915a4e6ea7ccbeb8e5e5eecef29a1d0d90f053183726a424b6d3b07325",
        )
        self.assertEqual(
            verify.mnemonic_from_entropy(verify.hashlib.sha256(text.encode()).digest()[:16]),
            "unveil nice picture region tragic fault cream strike tourist control recipe tourist",
        )

    def test_cards_domain_and_duplicates(self):
        cards = [rank + suit for suit in "CDHS" for rank in "A23456789TJQK"][:25]
        self.assertTrue(verify.transcript("cards", cards, 25).startswith("cards-v1:"))
        with self.assertRaisesRegex(ValueError, "distinct"):
            verify.transcript("cards", ["AC"] * 25, 25)

    def test_checksum_completion_all_zero(self):
        prefix = " ".join(["abandon"] * 11)
        wl = verify.words()
        packed = 0
        for word in prefix.split():
            packed = (packed << 11) | wl.index(word)
        entropy = ((packed << 7) | int("0000000", 2)).to_bytes(16, "big")
        self.assertEqual(verify.mnemonic_from_entropy(entropy), self.MNEMONIC)

    def test_all_checksum_completion_tails_are_unique_and_valid(self):
        wl = verify.words()
        for prefix_count, missing in ((11, 7), (23, 3)):
            packed = 0
            for _ in range(prefix_count):
                packed = (packed << 11) | wl.index("abandon")
            results = set()
            for tail in range(1 << missing):
                entropy = ((packed << missing) | tail).to_bytes(16 if missing == 7 else 32, "big")
                mnemonic = verify.mnemonic_from_entropy(entropy)
                self.assertEqual(verify.mnemonic_entropy(mnemonic), entropy)
                results.add(mnemonic.split()[-1])
            self.assertEqual(len(results), 1 << missing)

    def test_firmware_entropy_core_has_no_rng_call(self):
        source = (Path(__file__).parents[1] / "main/seedtool_core.c").read_text()
        for forbidden in ("get_random", "esp_random", "esp_fill_random", "randombytes"):
            self.assertNotIn(forbidden, source)

    def test_firmware_uses_rng_only_for_secp_blinding(self):
        root = Path(__file__).parents[1]
        app = (root / "main/seedtool_app.c").read_text()
        platform = (root / "main/seedtool_platform_esp.c").read_text()
        self.assertEqual(app.count("seedtool_platform_random"), 1)
        self.assertEqual(platform.count("esp_fill_random"), 1)
        self.assertIn("wally_secp_randomize", app)

    def test_word_entry_never_consults_the_rng(self):
        # Every screen in the word-entry path must be a pure function of what
        # the user typed. Randomising the initial key or the suggestion order
        # would put the device RNG back into the one path this tool exists to
        # keep it out of.
        root = Path(__file__).parents[1]
        for name in ("main/seedtool_wordlist.c", "main/seedtool_render.c"):
            source = (root / name).read_text()
            for forbidden in ("random", "rand(", "esp_fill"):
                self.assertNotIn(forbidden, source, name)

    def test_two_button_navigation_uses_a_chord_to_select(self):
        root = Path(__file__).parents[1]
        platform = (root / "main/seedtool_platform_esp.c").read_text()
        header = (root / "main/seedtool_platform.h").read_text()
        for key in ("KEY_PREV", "KEY_NEXT", "KEY_SELECT", "KEY_TIMEOUT"):
            self.assertIn(key, header)
        # Both buttons together stand in for the select button the board lacks,
        # and holding both must not repeat that select.
        self.assertIn("left_seen && right_seen", platform)
        self.assertIn("left_seen != right_seen", platform)

    def test_passphrase_keyboards_cover_printable_ascii(self):
        source = (Path(__file__).parents[1] / "main/seedtool_app.c").read_text()
        layouts = source.split("passphrase_layouts[PASSPHRASE_PAGES] = {", 1)[1].split("};", 1)[0]
        covered = set()
        for line in layouts.splitlines():
            line = line.strip()
            if not line.startswith('"'):
                continue
            body = line[1 : line.rindex('"')]
            covered.update(body.replace("\\n", "").replace("\\b", "").replace("\\t", "").replace("\\r", "")
                           .replace('\\"', '"').replace("\\\\", "\\"))
        self.assertEqual(covered, {chr(c) for c in range(0x20, 0x7F)})

    def test_host_simulator_reuses_firmware_logic_and_renderer(self):
        cmake = (Path(__file__).parents[1] / "host/CMakeLists.txt").read_text()
        for shared in ("seedtool_app.c", "seedtool_core.c", "seedtool_render.c", "qrcode.c"):
            self.assertIn(shared, cmake)
        self.assertNotIn("origo_verify.py", cmake)

    def test_project_has_a_private_minimal_component_graph(self):
        cmake = (Path(__file__).parents[1] / "main/CMakeLists.txt").read_text()
        for forbidden in (
            "../../main",
            "esp32_deflate",
            " esp_adc",
            " ledc",
            " driver ",
            "BigFont",
            "DejaVu",
            "jade_symbols",
        ):
            self.assertNotIn(forbidden, cmake)
        self.assertIn("fonts/DefaultFont.c", cmake)
        self.assertIn("fonts/Ubuntu16.c", cmake)

    def test_qr_is_locked_to_version_five(self):
        source = (Path(__file__).parents[1] / "main/qrcode.c").read_text()
        cmake = (Path(__file__).parents[1] / "main/CMakeLists.txt").read_text()
        self.assertIn("LOCK_VERSION=5", cmake)
        self.assertIn("LOCK_VERSION == 5", source)
        self.assertNotIn("LOCK_VERSION == 3", source)

    def test_bip84_and_bip86_published_vectors(self):
        fingerprint, addresses, accounts = verify.addresses(self.MNEMONIC, "", 0)
        self.assertEqual(fingerprint, "73c5da0a")
        self.assertEqual(addresses[84], "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu")
        self.assertEqual(addresses[86], "bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr")
        # Account xpubs as published in BIP84 and BIP86 themselves.
        self.assertEqual(
            accounts[84],
            "xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuyvz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhLv1VMrjPC7PW6V",
        )
        self.assertEqual(
            accounts[86],
            "xpub6BgBgsespWvERF3LHQu6CnqdvfEvtMcQjYrcRzx53QJjSxarj2afYWcLteoGVky7D3UKDP9QyrLprQ3VCECoY49yfdDEHGCtMMj92pReUsQ",
        )

    def test_base58check_handles_leading_zero_bytes(self):
        # Version byte plus an all-zero hash160: every leading zero byte must
        # survive as a literal '1', which plain integer encoding would drop.
        self.assertEqual(verify.base58check(bytes(21)), "1" * 21 + "4oLvT2")

    def test_input_ranges(self):
        with self.assertRaises(ValueError):
            verify.transcript("d6", ["1"] * 49 + ["7"], 50)
        with self.assertRaises(ValueError):
            verify.transcript("coin", ["1"] * 127, 128)


if __name__ == "__main__":
    unittest.main()
