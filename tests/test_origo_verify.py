import importlib.util
from pathlib import Path
import re
import sys
import unittest


MODULE = Path(__file__).parents[1] / "tools/origo_verify.py"
SPEC = importlib.util.spec_from_file_location("origo_verify", MODULE)
verify = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify)



APP_SOURCES = ("app_internal.h", "seedtool_app.c", "app_ui.c", "app_words.c", "app_wallet.c", "app_seed.c")


def app_source():
    """The firmware app's C source, all of its modules joined."""
    main = Path(__file__).parents[1] / "main"
    return "\n".join((main / name).read_text() for name in APP_SOURCES if (main / name).exists())

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
        # agreeing is the actual confidence here, not either one alone. The
        # chain step is BIP389's multipath <0;1>, whose three extra characters
        # are all inside BIP380's INPUT_CHARSET, so the checksum covers it
        # rather than rejecting it.
        fingerprint, _, accounts = verify.addresses(self.MNEMONIC, "", 0)
        self.assertEqual(
            verify.descriptor(fingerprint, 84, accounts[84]),
            f"wpkh([{fingerprint}/84'/0'/0']{accounts[84]}/<0;1>/*)#hpg6d6w2",
        )
        # The pre-BIP389 fallback is not a new string to be trusted on its own:
        # it is byte for byte the descriptor this firmware exported before the
        # multipath change, checksum included, which is what makes it a safe
        # thing to hand a wallet that rejects <0;1>.
        self.assertEqual(
            verify.descriptor(fingerprint, 84, accounts[84], multipath=False),
            f"wpkh([{fingerprint}/84'/0'/0']{accounts[84]}/0/*)#wc3n3van",
        )

    def test_change_branch_matches_the_published_bip84_vector(self):
        # BIP84 publishes both branches for this mnemonic, so the change side
        # is pinned to the spec's own vector rather than to whatever this
        # implementation happens to produce. The same assertion the firmware
        # self-test makes, from the independent implementation.
        _, receive, _ = verify.addresses(self.MNEMONIC, "", 0, chain=0)
        _, change, _ = verify.addresses(self.MNEMONIC, "", 0, chain=1)
        self.assertEqual(receive[84], "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu")
        self.assertEqual(change[84], "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el")
        # Threaded, not ignored: every type must differ across the branches at
        # the same index, taproot included.
        for purpose in (84, 86):
            self.assertNotEqual(receive[purpose], change[purpose])

    def test_coin_words_reach_the_published_vectors_and_agree_with_complete(self):
        # The device's Flip-each-word method packs eleven flips per word
        # straight into the entropy, with no hash - so the verifier has to be
        # able to reproduce it from the flips alone, or a seed made that way is
        # one the docs' "recompute it independently" promise does not cover.
        #
        # All-zero flips are the anchor: 121 zero bits plus seven more are 128
        # zero entropy bits, which BIP39 publishes as abandon x11 + about. That
        # fixes the whole chain, checksum included, to something outside this
        # project.
        wl = verify.words()

        def mnemonic_from_flips(flips, words):
            count = 11 if words == 12 else 23
            idx = [int(flips[i * 11 : (i + 1) * 11], 2) for i in range(count)]
            tail = flips[count * 11 :]
            packed = int("".join(f"{i:011b}" for i in idx), 2)
            entropy = ((packed << len(tail)) | int(tail, 2)).to_bytes(16 if words == 12 else 32, "big")
            return verify.mnemonic_from_entropy(entropy), [wl[i] for i in idx], tail

        self.assertEqual(mnemonic_from_flips("0" * 128, 12)[0], self.MNEMONIC)
        self.assertEqual(mnemonic_from_flips("0" * 256, 24)[0], self.MNEMONIC_24)

        # Eleven bits are one index and nothing past it: the top of the
        # wordlist must be reachable and must not overflow into a twelfth bit.
        self.assertEqual(len(wl), 1 << 11)
        self.assertEqual(mnemonic_from_flips("1" * 121 + "0" * 7, 12)[1][0], "zoo")

        # And the same flips must reach the same mnemonic through `complete`,
        # which is the route a reader takes when converting by hand: the two
        # are the same arithmetic entered from different ends, so a divergence
        # would mean one of them is lying about what the device did.
        flips = "01100110011" * 11 + "0110011"
        expected, words11, tail = mnemonic_from_flips(flips, 12)
        self.assertEqual(words11[0], "grid")
        packed = 0
        for word in words11:
            packed = (packed << 11) | wl.index(word)
        entropy = ((packed << len(tail)) | int(tail, 2)).to_bytes(16, "big")
        self.assertEqual(verify.mnemonic_from_entropy(entropy), expected)

    def test_seedpicker_tickets_match_seedpickers_own_last_word(self):
        # A ticket and a die name word number (ticket - 1) * 6 + die. The
        # worked example in docs/entropy.md, checked here end to end through
        # the `tickets` subcommand; with three tails it must give the word
        # SeedPicker's calculator gives, which takes the alphabetically first
        # valid 24th word. That calculator is reimplemented below from its
        # source (lib/logic.js, firstChecksumWordAlphabetically) rather than
        # trusted to agree.
        import argparse, contextlib, io
        draws = ("57:4 201:1 342:2 1:1 118:6 300:3 23:5 171:2 256:4 89:6 14:3 330:1 77:2 142:5 5:6 "
                 "268:1 199:3 36:4 311:2 160:6 44:1 228:5 97:3").split()

        def run(bits, these=draws):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                verify.tickets(argparse.Namespace(draws=list(these), bits=bits))
            return out.getvalue()

        printed = run("000")
        self.assertIn("word  1: T57 + die 4  0340  clerk", printed)
        self.assertIn("word  3: T342 + die 2  2048  zoo", printed)
        mnemonic = printed.splitlines()[-1].split(None, 1)[1]
        self.assertEqual(mnemonic.split()[-2:], ["embark", "cabin"])
        self.assertTrue(run("101").rstrip().endswith("embark phrase"))

        wl = verify.words()
        prefix = mnemonic.split()[:23]

        def valid(candidate):
            try:
                verify.mnemonic_entropy(" ".join(prefix + [candidate]))
                return True
            except ValueError:
                return False

        self.assertEqual(next(w for w in wl if valid(w)), "cabin")
        self.assertEqual(verify.addresses(mnemonic, "", 0, 0, 0)[0], "60c2d222")

        # The device's keypad offers only digits that lead to T1..T342, so a
        # ticket out of range cannot be typed at all.
        self.assertIn("enter_number(title, TICKET_DIGITS, 1, SEEDTOOL_TICKETS, &ticket, tally)", app_source())

        # The four pairs past 2048, and anything out of range, are refused.
        for bad in ("342:3", "342:6", "343:1", "0:1", "1:7"):
            with self.assertRaises(ValueError):
                run("000", draws[:22] + [bad])

    def test_dice_words_match_the_firmware_vectors(self):
        # The D6 and D20 methods read faces as bits with no hash, so the
        # verifier must reproduce them from the rolls alone. The vectors are
        # the ones tests/test_core.c pins for seedtool_die_word: all-4 D6
        # rolls are zoo, a last 2 keeps only its first bit (zone, not zoo),
        # and D20 16 1 10 is word 1925.
        import argparse, contextlib, io

        def run(die, rolls, bits):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                verify.dice(argparse.Namespace(die=die, rolls=[str(r) for r in rolls], bits=bits))
            return out.getvalue()

        # Eleven all-1 words and seven tails are 128 zero bits: BIP39's own
        # vector, so the packing is fixed to something outside this project.
        self.assertTrue(run("d6", [1] * 66, "0000000").rstrip().endswith(self.MNEMONIC))
        self.assertTrue(run("d20", [1] * 69, "000").rstrip().endswith(self.MNEMONIC_24))
        self.assertIn("word  1: 4, 4, 4, 4, 4, 4  2048  zoo", run("d6", [4] * 66, "0000000"))
        self.assertIn("word  1: 4, 4, 4, 4, 4, 2  2047  zone", run("d6", [4, 4, 4, 4, 4, 2] + [1] * 60, "0000000"))
        self.assertIn("word  1: 16, 1, 10  1925  vacuum", run("d20", [16, 1, 10] + [1] * 30, "0000000"))

        # The faces that do not count are rolled again, never entered.
        for die, rolls in (("d6", [5] + [1] * 65), ("d20", [17] + [1] * 32), ("d6", [1] * 65)):
            with self.assertRaises(ValueError):
                run(die, rolls, "0000000")

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

    def test_segwit_encoding_uses_the_right_constant_per_witness_version(self):
        # bech32 and bech32m differ only in the constant xored into the
        # checksum, and using v0's for a v1 program is the classic way to
        # produce an address wallets reject. The published BIP84/BIP86 vectors
        # elsewhere in this file already pin one address of each kind; this
        # pins the rule they are instances of, for programs those vectors do
        # not cover.
        program20, program32 = bytes(range(20)), bytes(range(32))
        v0 = verify.segwit(program20, 0)
        v1 = verify.segwit(program32, 1)
        self.assertTrue(v0.startswith("bc1q"))
        self.assertTrue(v1.startswith("bc1p"))
        # The same program at two versions must not share a checksum, which is
        # what a single hard-coded constant would produce.
        a = verify.segwit(program32, 0)
        b = verify.segwit(program32, 1)
        self.assertNotEqual(a[-6:], b[-6:])

    def test_convertbits_pads_the_final_group(self):
        # 8-to-5 regrouping leaves a remainder unless the input is a multiple
        # of five bytes; the leftover bits must be left-shifted into a final
        # group rather than dropped. A 20-byte hash160 is 160 bits = exactly
        # 32 groups, but a 32-byte taproot key is 256 bits = 51 groups plus
        # one bit, and losing that bit changes the address.
        self.assertEqual(len(verify.convertbits(bytes(20))), 32)
        self.assertEqual(len(verify.convertbits(bytes(32))), 52)
        # The trailing bit really is carried, not zeroed by accident: a
        # program differing only in its last bit must convert differently.
        low = verify.convertbits(bytes(31) + bytes([0x00]))
        high = verify.convertbits(bytes(31) + bytes([0x01]))
        self.assertNotEqual(low, high)

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

    def test_firmware_entropy_core_has_no_rng_call(self):
        source = (Path(__file__).parents[1] / "main/seedtool_core.c").read_text()
        for forbidden in ("get_random", "esp_random", "esp_fill_random", "randombytes"):
            self.assertNotIn(forbidden, source)

    def test_firmware_uses_rng_only_for_secp_blinding(self):
        root = Path(__file__).parents[1]
        app = app_source()
        platform = (root / "main/seedtool_platform_esp.c").read_text()
        self.assertEqual(app.count("seedtool_platform_random"), 1)
        self.assertEqual(platform.count("esp_fill_random"), 1)
        self.assertIn("wally_secp_randomize", app)

    # Names that mean a buffer holds seed material. Deliberately a list of
    # patterns rather than a list of buffers: a new buffer called `passphrase`
    # is covered the day it is written, where a fixed inventory would have to
    # be remembered. A new *kind* of secret under a name not listed here is the
    # one case that escapes, so adding a pattern is part of adding one.
    # Two rounds of additions have come from reviewing other people's screens
    # rather than from this list being thought through: `indices` and `bits`
    # hold a mnemonic just as plainly as `words` does, and both were wiped by
    # the author rather than by anything here noticing they had to be. That is
    # the failure mode to expect - the list lags the code - so it is worth
    # widening on sight rather than when something goes wrong.
    SECRET_BUFFER_NAMES = (
        "mnemonic", "passphrase", "seed", "entropy", "word", "words", "stem",
        "flips", "coin_bits", "completed", "attempt", "confirmation", "xpub",
        # A mnemonic by another name: word indices, the bits they are packed
        # from, and the strings a screen shows them in.
        "indices", "bits", "tail", "history", "bitline", "prefix", "digits",
        "picked", "drawn", "named", "made", "rows",
    )

    @staticmethod
    def _c_functions(source):
        """(name, body) for each function definition, by brace counting."""
        for match in re.finditer(r"^(?:static\s+)?[\w ]+?\**(\w+)\([^;]*?\)\s*\{", source, re.M | re.S):
            depth, i = 0, match.end() - 1
            while i < len(source):
                if source[i] == "{":
                    depth += 1
                elif source[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            yield match.group(1), source[match.end():i]

    def test_every_secret_buffer_is_wiped_in_the_function_that_holds_it(self):
        # docs/safety.md promises that every screen holding seed material
        # wipes its buffers when it is left. seedtool_zero is called
        # over a hundred times to that end and, until this test, not one of
        # those calls was verified by anything - the promise rested entirely on
        # each author remembering. Two real gaps of exactly this shape were
        # found by hand in c330a74, which is the argument for checking it.
        #
        # What this pins is narrow and deliberately so: a local array whose
        # name says it holds a secret must be passed to seedtool_zero somewhere
        # in the same function. It cannot prove every *path* wipes - that needs
        # the compiler, not a regex - but it does catch the buffer that is
        # never wiped at all, which is what both real gaps were.
        root = Path(__file__).parents[1]
        pattern = re.compile(
            r"^\s+(?:char|uint8_t|uint16_t)\s+(\w+)\s*\[", re.M)
        unwiped = []
        for name in (*(f"main/{n}" for n in APP_SOURCES), "main/seedtool_core.c"):
            if not (root / name).exists():
                continue
            source = (root / name).read_text()
            for func, body in self._c_functions(source):
                for buf in pattern.findall(body):
                    if buf not in self.SECRET_BUFFER_NAMES:
                        continue
                    if not re.search(rf"seedtool_zero\(\s*&?{re.escape(buf)}\b", body):
                        unwiped.append(f"{name}:{func}() leaves `{buf}` unwiped")
        self.assertEqual(unwiped, [], "; ".join(unwiped))

    def test_the_wipe_check_would_notice_an_unwiped_buffer(self):
        # A checker that silently matches nothing passes just as quietly as one
        # that works, and this one is a regex over C - exactly the kind that
        # rots into a no-op. So: it must find the buffers that are there, and
        # it must fail on one that is not wiped.
        found = [
            buf
            for func, body in self._c_functions(app_source())
            for buf in re.findall(r"^\s+(?:char|uint8_t)\s+(\w+)\s*\[", body, re.M)
            if buf in self.SECRET_BUFFER_NAMES
        ]
        self.assertGreaterEqual(len(found), 8, f"the scan found only {found}")
        planted = "static void f(void)\n{\n    char passphrase[64] = { 0 };\n    use(passphrase);\n}\n"
        functions = list(self._c_functions(planted))
        self.assertEqual(len(functions), 1)
        self.assertNotIn("seedtool_zero", functions[0][1])

    def test_word_entry_never_consults_the_rng(self):
        # Every screen in the word-entry path must be a pure function of what
        # the user typed. Randomising the initial key or the suggestion order
        # would put the device RNG back into the one path this tool exists to
        # keep it out of.
        root = Path(__file__).parents[1]
        for name in ("main/seedtool_wordlist.c", "main/seedtool_render.c", "main/render_entry.c", "main/render_qr.c"):
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

    def test_go_to_index_reads_the_list_only_below_its_length(self):
        # "Go to index" reads the static address list directly below
        # ADDRESS_SHOWN_ROWS and derives anything else through the core, which
        # range-checks it. The read is bounded by one comparison a compiler
        # cannot see the purpose of, so it is pinned here, with the assertion
        # holding the keypad width and the derived range together.
        app = app_source()
        self.assertIn("if (index < ADDRESS_SHOWN_ROWS) {", app)
        self.assertIn("ADDRESS_INDEX_DIGITS == 4 && SEEDTOOL_MAX_ADDRESS_INDEX == 9999", app)
        self.assertIn("ADDRESS_INDEX_DIGITS <= NUMBER_MAX_DIGITS", app)
        # The keypad's own cap is what the assertion above is asserting about;
        # if it stops bounding the digits, the pair being pinned means nothing.
        self.assertIn("digits_len < max_digits", app)
        self.assertIn('enter_number("Go to index", ADDRESS_INDEX_DIGITS', app)

    def test_firmware_emits_the_branch_it_is_tested_for(self):
        # Everything else about the chain level is tested through the core
        # functions, which the screens are free to call correctly and then
        # display something else entirely. Both the descriptor body and the
        # per-address title are built by their own snprintf in the app, so a
        # revert to a receive-only string there passes the C self-test, all of
        # these tests and the build - the multipath export would simply be
        # gone. Same reasoning, and same remedy, as the Go to index pin above.
        # assertTrue/assertFalse rather than assertIn/assertNotIn: the haystack
        # is the whole 2000-line file, and unittest prints the haystack on
        # failure - a hundred kilobytes of C around the one line that matters.
        root = Path(__file__).parents[1]
        app = app_source()
        for fragment, why in (
            ("/<0;1>/*%s", "the descriptor body is no longer BIP389 multipath"),
            ("/<0;1>/*))#12345678", "DESCRIPTOR_LEN no longer bounds the longest multipath body"),
            # The address list's title is the path prefix its rows share, and
            # each address's own path is that prefix plus an index - so the
            # branch is carried once, by the prefix, and both the title and
            # every row depend on it.
            ("\"m/%u'/0'/%u'/%u\"", "the address path prefix no longer carries the branch"),
            ("\"%s/%u\"", "an address path is no longer built from the shared prefix"),
        ):
            self.assertTrue(fragment in app, why)
        for fragment, why in (
            ("]%s/0/*", "the descriptor body reverted to receive-only"),
            ("\"m/%u'/0'/%u'/0/%u\"", "the address title reverted to a hardcoded receive branch"),
        ):
            self.assertFalse(fragment in app, why)

    def test_number_entry_narrows_by_the_same_set_the_letters_do(self):
        # The checksum filter is proved exact by the C self-test, but that
        # proves the *function*, not that the screen calls it. enter_word_number
        # is the screen, and a rewrite of it that passed NULL instead of the
        # allowed set would leave the number pad accepting last words
        # the letter keyboard refuses - the exact asymmetry the filter exists to
        # prevent, and invisible to every other test here, since the function it
        # stopped calling still works perfectly.
        #
        # That is not hypothetical: it is what a rewrite of this screen did.
        source = app_source()
        start = source.index("bool enter_word_number(")
        body = source[start : source.index("\n}\n", start)]
        self.assertIn("const seedtool_wordset_t* allowed", body, "enter_word_number no longer takes the allowed set")
        for narrowed in ("seedtool_next_digits_in(allowed", "seedtool_word_number_in(allowed"):
            self.assertIn(narrowed, body, f"enter_word_number no longer calls {narrowed}")

    def test_warning_screens_do_not_preselect_the_way_forward(self):
        # A screen whose purpose is the warning on it opens on the back arrow,
        # not on the confirm bar: the way forward should not be one press away
        # on a screen that exists to say this may be a bad idea. The rule was
        # applied to three screens and missed on two carrying the same "reveals
        # every address" warning, which is the kind of drift a convention takes
        # when nothing checks it.
        #
        # nav_acknowledge's last argument is that choice. Matched by the text
        # the screen shows rather than by a list of call sites, so a new screen
        # making one of these claims is covered the day it is written.
        source = app_source()
        warnings = (
            "A photo reveals every address",
            "A photo = total loss of funds",
            # confirm_word's discard question, whose warning ("These flips
            # are lost", "This draw is lost") arrives as `lost`.
            "question, subject, lost",
        )
        calls = re.findall(r"nav_acknowledge\(\s*(.*?)\)\s*\)", source, re.S)
        calls += re.findall(r"nav_screen\(([^()]*)\)", source, re.S)
        seen = 0
        for call in calls:
            if not any(w in call for w in warnings):
                continue
            seen += 1
            # The last argument before the closing paren is start_on_back.
            self.assertTrue(
                call.rstrip().rstrip(")").rstrip().endswith("true"),
                f"a warning screen does not open on the arrow: {' '.join(call.split())[:90]}",
            )
        self.assertGreaterEqual(seen, 4, f"the warning-text scan found only {seen} screens; it has stopped matching")

    def test_passphrase_keyboards_cover_printable_ascii(self):
        # Each arrangement on its own: a character only the QWERTY pages held
        # would leave a passphrase untypeable for a reader on ABC.
        source = (Path(__file__).parents[1] / "main/render_entry.c").read_text()
        for name in ("seedtool_passphrase_layouts", "seedtool_passphrase_layouts_abc"):
            anchor = f"{name}[SEEDTOOL_PASSPHRASE_PAGES] = {{"
            self.assertIn(anchor, source, f"{name} is not where this test looks for it")
            layouts = source.split(anchor, 1)[1].split("};", 1)[0]
            covered = set()
            for line in layouts.splitlines():
                line = line.strip()
                if not line.startswith('"'):
                    continue
                body = line[1 : line.rindex('"')]
                covered.update(body.replace("\\n", "").replace("\\b", "").replace("\\t", "").replace("\\r", "")
                               .replace('\\"', '"').replace("\\\\", "\\"))
            self.assertEqual(covered, {chr(c) for c in range(0x20, 0x7F)}, name)

    def test_logo_c_file_is_what_the_cells_generate(self):
        # The C file is committed and the cells are what gets edited, so the two
        # can drift; regenerating into a scratch file must give the same bytes.
        import subprocess, tempfile
        root = Path(__file__).parents[1]
        with tempfile.TemporaryDirectory() as scratch:
            c_file, png = Path(scratch) / "logo.c", Path(scratch) / "logo.png"
            subprocess.run(
                [sys.executable, "-I", str(root / "tools/make_logo.py"), "--c", str(c_file), "--png", str(png)],
                check=True, capture_output=True,
            )
            self.assertEqual(c_file.read_text(), (root / "main/seedtool_logo.c").read_text())
            self.assertEqual(png.read_bytes(), (root / "assets/logo.png").read_bytes())

    def test_logo_asset_matches_its_header(self):
        # The artwork is generated from assets/logo.txt by tools/make_logo.py,
        # but what ships is the committed C file. A header and an array that
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
        flush = display.split("void seedtool_display_flush(void)", 1)[1].split("\n}", 1)[0]
        self.assertIn("transmit_pixels()", flush)
        self.assertNotIn("seedtool_render_pixels()", flush)

    def test_host_simulator_reuses_firmware_logic_and_renderer(self):
        cmake = (Path(__file__).parents[1] / "host/CMakeLists.txt").read_text()
        for shared in ("APP_SOURCES", "seedtool_core.c", "seedtool_render.c", "render_entry.c", "render_qr.c", "qrcode.c"):
            self.assertIn(shared, cmake)
        self.assertNotIn("origo_verify.py", cmake)

    def test_project_has_a_private_minimal_component_graph(self):
        cmake = (Path(__file__).parents[1] / "main/CMakeLists.txt").read_text()
        for forbidden in (
            "../../main",
            "esp32_deflate",
            " esp_adc",
            " driver ",
            "BigFont",
            "DejaVu",
            "jade_symbols",
        ):
            self.assertNotIn(forbidden, cmake)
        self.assertIn("fonts/DefaultFont.c", cmake)
        self.assertIn("fonts/UbuntuRegular16.c", cmake)
        self.assertIn("fonts/UbuntuMedium16.c", cmake)

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
        render = (root / "main/render_qr.c").read_text()

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

    def test_bip44_and_bip49_vectors(self):
        # The first receive addresses every BIP39 reference wallet derives for
        # this mnemonic on the legacy and nested-segwit paths.
        _, addresses, accounts = verify.addresses(self.MNEMONIC, "", 0)
        self.assertEqual(addresses[44], "1LqBGSKuX5yYUonjxT5qGfpUsXKYYWeabA")
        self.assertEqual(addresses[49], "37VucYSaXLCAsxYyAPfbSi9eh4iEcbShgf")
        # SLIP-132 ypub for the BIP49 account: the xpub's 78 bytes under
        # ypub's version.
        self.assertEqual(
            accounts["49y"],
            "ypub6Ww3ibxVfGzLrAH1PNcjyAWenMTbbAosGNB6VvmSEgytSER9azLDWCxoJwW7Ke7icmizBMXrzBx9979FfaHxHcrArf3zbeJJJUZPf663zsP",
        )
        fingerprint = "73c5da0a"
        self.assertTrue(verify.descriptor(fingerprint, 44, accounts[44]).startswith(f"pkh([{fingerprint}/44'/0'/0']"))
        nested = verify.descriptor(fingerprint, 49, accounts[49])
        self.assertTrue(nested.startswith(f"sh(wpkh([{fingerprint}/49'/0'/0']"))
        self.assertIn("/<0;1>/*))#", nested)

    def test_base58check_handles_leading_zero_bytes(self):
        # Version byte plus an all-zero hash160: every leading zero byte must
        # survive as a literal '1', which plain integer encoding would drop.
        self.assertEqual(verify.base58check(bytes(21)), "1" * 21 + "4oLvT2")


if __name__ == "__main__":
    unittest.main()
