import importlib.util
from pathlib import Path
import re
import unittest


MODULE = Path(__file__).parents[1] / "tools/origo_verify.py"
SPEC = importlib.util.spec_from_file_location("origo_verify", MODULE)
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)


class SeedToolVerifierTests(unittest.TestCase):
    MNEMONIC = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
    MNEMONIC_24 = " ".join(["abandon"] * 23 + ["art"])

    def test_bip39_zero_vector(self):
        self.assertEqual(verify.mnemonic_from_entropy(bytes(16)), self.MNEMONIC)
        self.assertEqual(verify.mnemonic_entropy(self.MNEMONIC), bytes(16))

    def test_bad_checksum_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "checksum"):
            verify.mnemonic_entropy(self.MNEMONIC.replace("about", "abandon"))

    def test_compact_seedqr_zero_vector(self):
        self.assertEqual(verify.compact_seedqr_payload(self.MNEMONIC), bytes(16))

    def test_compact_seedqr_24_word_zero_vector(self):
        self.assertEqual(verify.mnemonic_from_entropy(bytes(32)), self.MNEMONIC_24)
        self.assertEqual(verify.compact_seedqr_payload(self.MNEMONIC_24), bytes(32))

    def test_compact_seedqr_rejects_bad_checksum(self):
        with self.assertRaisesRegex(ValueError, "checksum"):
            verify.compact_seedqr_payload(self.MNEMONIC.replace("about", "abandon"))

    def test_word_numbers_are_one_based(self):
        self.assertEqual(verify.word_numbers(self.MNEMONIC), [1] * 11 + [4])
        with self.assertRaisesRegex(ValueError, "BIP39"):
            verify.word_numbers("abandon notaword")

    def test_every_word_number_round_trips(self):
        # The device converts a typed number to a word by subtracting one. A
        # base that disagreed here would produce a different seed in silence,
        # so the whole list is checked rather than an example of it.
        wl = verify.words()
        for index, word in enumerate(wl):
            self.assertEqual(verify.word_numbers(word), [index + 1])
            self.assertEqual(wl[verify.word_numbers(word)[0] - 1], word)
        self.assertEqual(verify.word_numbers("abandon zoo"), [1, 2048])

    def test_account_qr_payload_fits_the_encoder(self):
        # The device pins its QR encoder to version 6. A payload one byte over
        # capacity does not degrade: nothing is drawn at all.
        _, _, accounts = verify.addresses(self.MNEMONIC, "", 0)
        for purpose in (84, 86):
            payload = verify.account_qr_payload("73c5da0a", purpose, accounts[purpose])
            self.assertEqual(payload, f"[73c5da0a/{purpose}'/0'/0']{accounts[purpose]}")
            self.assertLessEqual(len(payload), verify.QR_CAPACITY)
        with self.assertRaisesRegex(ValueError, "capacity"):
            verify.account_qr_payload("73c5da0a", 84, "x" * verify.QR_CAPACITY)

    def test_account_index_changes_derivation_and_still_fits_the_qr(self):
        # Mirrors the firmware's own account-index self-test: a different
        # account must give a different xpub (the parameter is actually
        # threaded into the derivation, not silently ignored), and the
        # worst-case three-digit account still fits the same 134-byte
        # encoder the plain account-0 payload does - it lands right at that
        # ceiling rather than comfortably under it.
        _, _, account_zero = verify.addresses(self.MNEMONIC, "", 0, account_index=0)
        _, _, account_one = verify.addresses(self.MNEMONIC, "", 0, account_index=1)
        self.assertNotEqual(account_zero[84], account_one[84])
        fingerprint, _, accounts_max = verify.addresses(self.MNEMONIC, "", 0, account_index=999)
        payload = verify.account_qr_payload(fingerprint, 84, accounts_max[84], account=999)
        self.assertEqual(payload, f"[{fingerprint}/84'/0'/999']{accounts_max[84]}")
        self.assertLessEqual(len(payload), verify.QR_CAPACITY)

    def test_descriptor_checksum_matches_bip380_vector(self):
        # bip-0380.mediawiki's own published example, not a vector this
        # project invented for itself.
        self.assertEqual(verify.descriptor_checksum("raw(deadbeef)"), "89f8spxm")
        with self.assertRaisesRegex(ValueError, "checksum charset"):
            verify.descriptor_checksum("raw(dead\nbeef)")

    def test_descriptor_matches_the_published_bip84_vector(self):
        # The exact checksum this same descriptor got independently from the
        # firmware's own C implementation (seedtool_descriptor_checksum) -
        # two from-scratch implementations of bip-0380.mediawiki's pseudocode
        # agreeing is the actual confidence here, not either one alone.
        fingerprint, _, accounts = verify.addresses(self.MNEMONIC, "", 0)
        self.assertEqual(
            verify.descriptor(fingerprint, 84, accounts[84]),
            f"wpkh([{fingerprint}/84'/0'/0']{accounts[84]}/0/*)#wc3n3van",
        )

    def test_d6_transcript_pipeline_against_a_krux_vector(self):
        # Pins the pipeline - digits concatenated, SHA256, truncate, BIP39 -
        # against a run Krux produces the same mnemonic from, which is what
        # makes this vector worth keeping. It does NOT pin device interop:
        # Origo asks for exactly 60 D6 rolls for 12 words where Krux asks for
        # at least 50, so the 50-roll run below is one the device will no
        # longer accept. That divergence is deliberate (see the entropy
        # section of README.md) - Krux can absorb a marginal count by letting
        # the user keep rolling and by grading with a 2-bit tolerance, and
        # Origo, whose runs are fixed length, pays for the margin in rolls
        # instead. The count is passed explicitly here for that reason.
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

    def test_shannon_bits_of_a_uniform_and_a_constant_run(self):
        # An all-one-face run carries no information: every roll was foretold by
        # the last, so entropy is exactly zero regardless of how many there are.
        self.assertEqual(verify.shannon_bits("d6", [3] * 50), 0)
        # A perfectly even spread over all six faces is the maximum a D6 can
        # carry per roll: log2(6) bits each, six rolls here.
        self.assertEqual(verify.shannon_bits("d6", [1, 2, 3, 4, 5, 6]), int(6 * verify.math.log2(6)))
        with self.assertRaisesRegex(ValueError, "1..6"):
            verify.shannon_bits("d6", [7])

    def test_pattern_detected_on_an_arithmetic_run(self):
        # Repeating 1..6 in order is the lazy way to fake fifty rolls: every
        # face appears equally often (high Shannon entropy by count alone) but
        # the sequence is entirely predictable, which only the derivative check
        # below catches.
        run = [1, 2, 3, 4, 5, 6] * 8 + [1, 2]
        self.assertEqual(len(run), 50)
        self.assertTrue(verify.pattern_detected("d6", run))
        self.assertGreaterEqual(verify.shannon_bits("d6", run), 128)
        # Too few rolls to judge either way.
        self.assertFalse(verify.pattern_detected("d6", [1, 2, 3, 4, 5, 6, 1, 2, 3]))

    def test_cards_domain_and_duplicates(self):
        cards = [rank + suit for suit in "CDHS" for rank in "A23456789TJQK"][:25]
        self.assertTrue(verify.transcript("cards", cards, 25).startswith("cards-v1:"))
        with self.assertRaisesRegex(ValueError, "distinct"):
            verify.transcript("cards", ["AC"] * 25, 25)

    def test_coin_is_a_two_sided_die(self):
        # A coin flip is read as a two-sided die, 1/2 rather than the 0/1 the
        # transcript itself uses - coin_faces does that remap.
        self.assertEqual(verify.coin_faces(["H"] * 10), [2] * 10)
        self.assertEqual(verify.coin_faces(["tails", "Heads", "0", "1"]), [1, 2, 1, 2])
        with self.assertRaisesRegex(ValueError, "H/T"):
            verify.coin_faces(["X"])
        # All-heads carries no information, same shape as an all-one-face die.
        self.assertEqual(verify.shannon_bits("coin", verify.coin_faces(["H"] * 128)), 0)
        self.assertTrue(verify.pattern_detected("coin", verify.coin_faces(["H"] * 128)))
        # An alternating run is the coin equivalent of counting through a
        # die's faces: maximum entropy by count, but entirely predictable.
        alternating = verify.coin_faces(["H", "T"] * 64)
        self.assertEqual(verify.shannon_bits("coin", alternating), 128)
        self.assertTrue(verify.pattern_detected("coin", alternating))

    def test_card_entropy_bits_is_exact_not_estimated(self):
        # Cards are drawn without replacement, so this is exact rather than
        # estimated: no rolls needed to reach zero, and no upper bound but
        # the deck itself.
        self.assertEqual(verify.card_entropy_bits(0), 0)
        # The required draw count for a 12-word seed clears the 128-bit
        # minimum by construction.
        self.assertGreaterEqual(verify.card_entropy_bits(25), 128)
        # Non-decreasing over the whole deck; the very last card adds
        # nothing, since the other 51 already determine it.
        bits = [verify.card_entropy_bits(n) for n in range(53)]
        self.assertEqual(bits, sorted(bits))
        with self.assertRaisesRegex(ValueError, "52"):
            verify.card_entropy_bits(53)

    def test_card_pattern_detected_on_an_arithmetic_run(self):
        # A fresh, unshuffled deck read off in order: rank climbs 0..12
        # within each suit before resetting, the card equivalent of counting
        # through a die's faces.
        cards = [rank + suit for suit in "CDHS" for rank in "A23456789TJQK"][:25]
        self.assertTrue(verify.card_pattern_detected(verify.card_values(cards)))
        # A full, honestly shuffled draw is not flagged. Ten cards, right at
        # the pattern check's minimum sample size, is too easily unlucky by
        # chance alone (the same small-sample noise PATTERN_MIN_ROLLS exists
        # to guard against for dice) to make a reliable test vector.
        shuffled = ["TC", "JD", "KD", "4C", "9D", "KH", "4D", "AS", "7D", "QC", "8S", "QD", "8H", "4H", "6H", "5S",
            "5C", "3H", "JC", "AH", "JH", "AC", "6S", "6D", "4S"]
        self.assertFalse(verify.card_pattern_detected(verify.card_values(shuffled)))
        # Too few draws to judge either way.
        self.assertFalse(verify.card_pattern_detected(verify.card_values(cards[:9])))

    def test_cards_replace_allows_repeats_and_is_a_52_sided_die(self):
        # Unlike "cards", a repeat is valid here: the card is returned and
        # the deck reshuffled before every draw, not set aside.
        self.assertTrue(verify.transcript("cards-replace", ["AC"] * 48, 48).startswith("cards-v1:"))
        # Graded the same way "coin" already reuses the dice machinery for a
        # two-sided die - here a genuine 52-sided one.
        faces = [v + 1 for v in verify.card_values(["AC"] * 48)]
        self.assertEqual(verify.shannon_bits("cards-replace", faces), 0)
        self.assertTrue(verify.pattern_detected("cards-replace", faces))
        with self.assertRaisesRegex(ValueError, "1..52"):
            verify.shannon_bits("cards-replace", [0])

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

    def test_pubkey_pads_a_short_x_coordinate(self):
        # The compressed form is a parity byte and exactly 32 bytes of x. An
        # x that happens to fit in 31 needs a leading zero, and dropping it
        # yields a 32-byte key that hashes to a different address than the
        # device derives. secret=153 is the first such key, found by search
        # rather than picked: its x is 31 bytes.
        x, _ = verify.mul(153)
        self.assertEqual((x.bit_length() + 7) // 8, 31)
        encoded = verify.pubkey(153)
        self.assertEqual(len(encoded), 33)
        self.assertEqual(encoded[1], 0)
        # Every key serialises to 33 bytes, short x or not.
        for secret in (1, 153, 2**160, verify.N - 1):
            self.assertEqual(len(verify.pubkey(secret)), 33)

    def test_child_derivation_rejects_a_tweak_at_the_curve_order(self):
        # BIP32 invalidates a child when parse256(IL) >= n as well as when the
        # result is zero, and libwally enforces both through
        # secp256k1_ec_seckey_tweak_add. Only the zero case was checked here,
        # so the device would have refused to derive while this tool returned
        # a key. The HMAC output has to be forced: no reachable input produces
        # such a tweak on purpose.
        node = verify.master(self.MNEMONIC, "")
        real_hmac_new = verify.hmac.new

        class ForcedDigest:
            def __init__(self, head):
                self._head = head

            def digest(self):
                return self._head + bytes(32)

        for head, expected in (
            (verify.N.to_bytes(32, "big"), "at the order"),
            ((verify.N + 1).to_bytes(32, "big"), "above the order"),
        ):
            with self.subTest(tweak=expected):
                verify.hmac.new = lambda *a, _h=head, **k: ForcedDigest(_h)
                try:
                    with self.assertRaises(ValueError):
                        verify.child_private(node, 0)
                finally:
                    verify.hmac.new = real_hmac_new
        # The real HMAC still derives normally afterwards.
        self.assertEqual(len(verify.pubkey(verify.child_private(node, 0)[0])), 33)

    def test_hardened_and_unhardened_indices_are_distinct_at_the_boundary(self):
        # 0x7fffffff is the last unhardened index and 0x80000000 the first
        # hardened one. They differ in which data the HMAC is fed - the
        # private key for hardened, the public point otherwise - so the two
        # must not derive the same child.
        node = verify.master(self.MNEMONIC, "")
        last_normal = verify.child_private(node, 0x7FFFFFFF)
        first_hardened = verify.child_private(node, 0x80000000)
        self.assertNotEqual(last_normal[0], first_hardened[0])
        self.assertNotEqual(last_normal[1], first_hardened[1])
        # An empty path is the identity, which is what makes derive() safe to
        # call with a prefix that happens to be empty.
        self.assertEqual(verify.derive(node, []), node)

    def test_descriptor_checksum_catches_a_single_character_change(self):
        # The BIP380 checksum exists to catch transcription slips, so rather
        # than pin a second literal it is checked for the property it is for:
        # no single-character edit anywhere in the descriptor may leave the
        # checksum unchanged.
        fingerprint, _, accounts = verify.addresses(self.MNEMONIC, "", 0)
        desc = verify.descriptor(fingerprint, 84, accounts[84])
        body, _, checksum = desc.rpartition("#")
        self.assertEqual(len(checksum), 8)
        for i, original in enumerate(body):
            replacement = "0" if original != "0" else "1"
            if replacement not in verify.DESCRIPTOR_INPUT_CHARSET:
                continue
            mutated = body[:i] + replacement + body[i + 1 :]
            self.assertNotEqual(
                verify.descriptor_checksum(mutated), checksum, f"undetected edit at index {i}"
            )

    def test_verifier_event_counts_match_the_firmware(self):
        # The verifier exists to check the device without trusting it, and
        # transcript() refuses any count but the expected one - so a table
        # that disagrees with seedtool_required_events() does not merely
        # report a wrong minimum, it makes every seed from that source
        # unverifiable. That shipped once: D20 was padded from 30/60 to 36/68
        # in the firmware and this table kept 30/60, so `generate d20` failed
        # with "expected exactly 30 entries, got 36" on a perfectly good run.
        source = (Path(__file__).parents[1] / "main/seedtool_core.c").read_text()
        body = source.split("size_t seedtool_required_events", 1)[1].split("\n}", 1)[0]
        names = {
            "SEEDTOOL_D6": "d6",
            "SEEDTOOL_D20": "d20",
            "SEEDTOOL_COIN": "coin",
            "SEEDTOOL_CARDS": "cards",
            "SEEDTOOL_CARDS_REPLACE": "cards-replace",
        }
        # Each arm reads `return words == N ? <that N's count> : <the other>;`
        # and N is 12 or 24 depending on which case reads more naturally in C.
        pattern = re.compile(
            r"case\s+(SEEDTOOL_\w+):.*?return\s+words\s*==\s*(12|24)\s*\?\s*(\d+)\s*:\s*(\d+);",
            re.DOTALL,
        )
        found = {}
        for case, pivot, when_pivot, otherwise in pattern.findall(body):
            twelve, twentyfour = (
                (when_pivot, otherwise) if pivot == "12" else (otherwise, when_pivot)
            )
            # 0 is how the C spells "this combination is not offered"; the
            # Python table spells the same thing None.
            found[names[case]] = tuple(int(n) or None for n in (twelve, twentyfour))
        self.assertEqual(set(found), set(verify.REQUIRED_EVENTS), "a source was added or renamed")
        self.assertEqual(found, verify.REQUIRED_EVENTS)

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

    def test_logo_asset_matches_its_header(self):
        # The artwork is generated by an authoring tool that needs Pillow, but
        # what ships is the committed C file. A header and an array that
        # disagreed would read past the end of the pixels while drawing.
        root = Path(__file__).parents[1]
        header = (root / "main/seedtool_logo.h").read_text()
        source = (root / "main/seedtool_logo.c").read_text()
        declared = {
            name: int(re.search(rf"#define SEEDTOOL_LOGO_{name} (\d+)", header).group(1))
            for name in ("WIDTH", "HEIGHT", "COLOURS")
        }
        self.assertEqual(declared["WIDTH"] % 2, 0, "an odd width would pad every row")

        palette = re.findall(r"0x[0-9a-f]{4},", source.split("palette")[1].split("};")[0])
        pixels = re.findall(r"0x[0-9a-f]{2},", source.split("pixels")[1].split("};")[0])
        self.assertEqual(len(palette), declared["COLOURS"])
        self.assertEqual(len(pixels), declared["WIDTH"] * declared["HEIGHT"] // 2)
        # The one picture in the firmware must still fit the panel.
        render = (root / "main/seedtool_render.h").read_text()
        width = int(re.search(r"#define SEEDTOOL_DISPLAY_WIDTH (\d+)", render).group(1))
        height = int(re.search(r"#define SEEDTOOL_DISPLAY_HEIGHT (\d+)", render).group(1))
        self.assertLessEqual(declared["WIDTH"], width)
        self.assertLess(declared["HEIGHT"], height)

    def test_panel_receives_pixels_in_wire_order(self):
        # The framebuffer is little-endian because the CPU is; the panel reads
        # each pixel most significant byte first. Nothing on the host exercises
        # this, since origo_sdl.c replaces the whole driver, so the wiring is
        # asserted structurally: without it, deleting the conversion would leave
        # every test passing and the device showing blue where orange belongs.
        display = (Path(__file__).parents[1] / "main/seedtool_display.c").read_text()
        self.assertIn("seedtool_render_wire_rows", display)
        flush = display.split("static void flush(void)", 1)[1].split("\n}", 1)[0]
        self.assertIn("transmit_pixels()", flush)
        self.assertNotIn("seedtool_render_pixels()", flush)

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

    def test_qr_max_version_is_shared_and_unlocked(self):
        # LOCK_VERSION used to pin the encoder to a single fixed version; that
        # was deliberately removed (git history: "Fix D6 roll count display
        # and draw Compact SeedQR at its true size") so Compact SeedQR draws
        # at the smallest version its own entropy needs instead of always at
        # the size the larger account-key QR requires - unlocking it costs
        # under 2 KiB of flash. Firmware and simulator must agree on *not*
        # locking it, and on the same upper bound (QR_VERSION), since that
        # bound is still what makes the account-key payload fit at all.
        root = Path(__file__).parents[1]
        firmware = (root / "main/CMakeLists.txt").read_text()
        host = (root / "host/CMakeLists.txt").read_text()
        render = (root / "main/seedtool_render.c").read_text()

        self.assertNotIn("LOCK_VERSION", firmware)
        self.assertNotIn("LOCK_VERSION", host)
        max_version = re.findall(r"#define QR_VERSION (\d+)", render)
        self.assertEqual(max_version, ["6"])
        self.assertGreaterEqual(verify.QR_CAPACITY, 131)

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
        # SLIP-132 zpub for the same BIP84 account: the identical key, xpub's
        # version bytes swapped for zpub's.
        self.assertEqual(
            accounts["84z"],
            "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs",
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
