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
        source = (Path(__file__).parents[1] / "main/seedtool_app.c").read_text()
        self.assertEqual(source.count("esp_fill_random"), 1)
        self.assertIn("wally_secp_randomize", source)

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
        fingerprint, addresses = verify.addresses(self.MNEMONIC, "", 0)
        self.assertEqual(fingerprint, "73c5da0a")
        self.assertEqual(addresses[84], "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu")
        self.assertEqual(addresses[86], "bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr")

    def test_input_ranges(self):
        with self.assertRaises(ValueError):
            verify.transcript("d6", ["1"] * 49 + ["7"], 50)
        with self.assertRaises(ValueError):
            verify.transcript("coin", ["1"] * 127, 128)


if __name__ == "__main__":
    unittest.main()
