#!/usr/bin/env python3
"""Independent, dependency-free verifier for Origo outputs.

Do not type a real mnemonic or passphrase into a network-connected computer.
Run this script from a trusted, offline environment.
"""

import argparse
import hashlib
import hmac
import math
from pathlib import Path
import sys

H = 0x80000000
P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)
CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
BASE58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
XPUB_VERSION = b"\x04\x88\xb2\x1e"
ZPUB_VERSION = b"\x04\xb2\x47\x46"  # SLIP-132, native segwit (BIP84) only
WORDLIST = Path(__file__).parents[1] / "components/libwally-core/upstream/src/data/wordlists/english.txt"


def words():
    result = WORDLIST.read_text(encoding="ascii").splitlines()
    if len(result) != 2048:
        raise RuntimeError("bundled BIP39 English wordlist is missing or invalid")
    return result


def mnemonic_from_entropy(entropy):
    wl = words()
    cs = len(entropy) * 8 // 32
    bits = int.from_bytes(entropy, "big") << cs
    bits |= hashlib.sha256(entropy).digest()[0] >> (8 - cs)
    return " ".join(wl[(bits >> shift) & 2047] for shift in range(len(entropy) * 8 + cs - 11, -1, -11))


def word_numbers(mnemonic):
    """One-based positions in the printed wordlist, as the device shows them.

    The encoding index is zero-based; a word number is one more than it. The
    device offers entry by these numbers, so they need a second implementation
    here like every other value it can display.
    """
    wl = words()
    try:
        return [wl.index(w) + 1 for w in mnemonic.split()]
    except ValueError as exc:
        raise ValueError("word is not in BIP39 English") from exc


"""Version 6 at ECC low, which is what the device encoder is pinned to."""
QR_CAPACITY = 134


def account_qr_payload(fingerprint, purpose, xpub, account=0):
    """Exactly the bytes the device puts in the account key QR.

    Key origin then xpub with no separator, which is what a watch-only wallet
    needs to import the account without being told the derivation path. A photo
    of this code reveals every address of the account, past and future.
    """
    payload = f"[{fingerprint}/{purpose}'/0'/{account}']{xpub}"
    if len(payload) > QR_CAPACITY:
        raise ValueError(f"payload is {len(payload)} bytes, over the {QR_CAPACITY}-byte QR capacity")
    return payload


"""bip-0380.mediawiki's own reference charsets and generator, transcribed
verbatim from the spec's own pseudocode - this is the whole checksum
algorithm, not an approximation of it."""
DESCRIPTOR_INPUT_CHARSET = (
    "0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\"\\ "
)
DESCRIPTOR_CHECKSUM_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
DESCRIPTOR_GENERATOR = [0xF5DEE51989, 0xA9FDCA3312, 0x1BAB10E32D, 0x3706B1677A, 0x644D626FFD]


def _descriptor_polymod(symbols):
    chk = 1
    for value in symbols:
        top = chk >> 35
        chk = ((chk & 0x7FFFFFFFF) << 5) ^ value
        for i in range(5):
            if (top >> i) & 1:
                chk ^= DESCRIPTOR_GENERATOR[i]
    return chk


def _descriptor_expand(s):
    groups = []
    symbols = []
    for c in s:
        if c not in DESCRIPTOR_INPUT_CHARSET:
            raise ValueError(f"{c!r} is outside the descriptor checksum charset")
        v = DESCRIPTOR_INPUT_CHARSET.find(c)
        symbols.append(v & 31)
        groups.append(v >> 5)
        if len(groups) == 3:
            symbols.append(groups[0] * 9 + groups[1] * 3 + groups[2])
            groups = []
    if len(groups) == 1:
        symbols.append(groups[0])
    elif len(groups) == 2:
        symbols.append(groups[0] * 3 + groups[1])
    return symbols


def descriptor_checksum(desc):
    """BIP380 output descriptor checksum: the 8 characters after desc's '#'."""
    symbols = _descriptor_expand(desc) + [0] * 8
    checksum = _descriptor_polymod(symbols) ^ 1
    return "".join(DESCRIPTOR_CHECKSUM_CHARSET[(checksum >> (5 * (7 - i))) & 31] for i in range(8))


def descriptor(fingerprint, purpose, xpub, account=0):
    """The full wpkh()/tr() output descriptor Origo's own Descriptor export
    shows, checksum included - script fragment matches the address type,
    always plain xpub (BIP380 has no notion of SLIP-132's zpub)."""
    fragment = "wpkh" if purpose == 84 else "tr"
    body = f"{fragment}([{fingerprint}/{purpose}'/0'/{account}']{xpub}/0/*)"
    return f"{body}#{descriptor_checksum(body)}"


def mnemonic_entropy(mnemonic):
    wl = words()
    try:
        idx = [wl.index(w) for w in mnemonic.split()]
    except ValueError as exc:
        raise ValueError("word is not in BIP39 English") from exc
    if len(idx) not in (12, 24):
        raise ValueError("mnemonic must contain 12 or 24 words")
    packed = 0
    for i in idx:
        packed = (packed << 11) | i
    cs = len(idx) // 3
    entropy = (packed >> cs).to_bytes((len(idx) * 11 - cs) // 8, "big")
    if packed & ((1 << cs) - 1) != hashlib.sha256(entropy).digest()[0] >> (8 - cs):
        raise ValueError("invalid BIP39 checksum")
    return entropy


def compact_seedqr_payload(mnemonic):
    """Exactly the bytes the device puts in a Compact SeedQR: the mnemonic's
    raw entropy, with no checksum bits and no word text. A photo of this code
    is total, irreversible compromise of every key the mnemonic can ever
    derive — unlike the account key QR, there is no narrower blast radius to
    warn about.
    """
    return mnemonic_entropy(mnemonic)


def transcript(kind, entries, count):
    if len(entries) != count:
        raise ValueError(f"expected exactly {count} entries, got {len(entries)}")
    if kind == "d6":
        vals = [int(x) for x in entries]
        if any(x < 1 or x > 6 for x in vals):
            raise ValueError("D6 values must be 1..6")
        return "".join(map(str, vals))
    if kind == "d20":
        vals = [int(x) for x in entries]
        if any(x < 1 or x > 20 for x in vals):
            raise ValueError("D20 values must be 1..20")
        return "-".join(map(str, vals))
    if kind == "coin":
        vals = [{"h": "1", "heads": "1", "1": "1", "t": "0", "tails": "0", "0": "0"}.get(x.lower()) for x in entries]
        if any(x is None for x in vals):
            raise ValueError("coins must be H/T or 1/0")
        return "".join(vals)
    # "cards" and "cards-replace" share the same rank+suit encoding; only
    # "cards" (drawn without replacement) rejects a repeat.
    ranks, suits = "A23456789TJQK", "CDHS"
    vals = [x.upper() for x in entries]
    allowed = {r + s for s in suits for r in ranks}
    if any(x not in allowed for x in vals):
        raise ValueError("cards must be rank+suit codes (AC..KS)")
    if kind == "cards" and len(set(vals)) != len(vals):
        raise ValueError("cards must be distinct rank+suit codes (AC..KS)")
    return "cards-v1:" + "".join(vals)


def shannon_bits(kind, values):
    """Shannon's entropy, in bits, of a D6/D20/coin roll sequence.

    A UI quality signal, not a cryptographic one: the mnemonic is generated
    from the transcript's SHA256 regardless of this value. A coin flip is a
    two-sided die, 1..2 like every other kind here - not the 0/1 the
    transcript itself uses; see coin_faces. Mirrors seedtool_dice_entropy_bits,
    adapted from Krux's dice-roll entropy screen (github.com/selfcustody/krux,
    src/krux/pages/new_mnemonic/dice_rolls.py).
    """
    sides = {"d6": 6, "d20": 20, "coin": 2, "cards-replace": 52}[kind]
    if any(v < 1 or v > sides for v in values):
        raise ValueError(f"{kind} values must be 1..{sides}")
    if not values:
        return 0
    counts = [0] * sides
    for v in values:
        counts[v - 1] += 1
    bits = 0.0
    for count in counts:
        if count:
            p = count / len(values)
            bits -= p * math.log2(p)
    return int(bits * len(values))


def derivative_pattern_detected(values_1indexed, sides):
    """Shared by pattern_detected and card_pattern_detected: whether a
    `sides`-valued 1-indexed sequence's consecutive differences look like an
    arithmetic run (an operator counting up or down through the faces)
    rather than random. Mirrors seedtool_core.c's derivative_pattern_detected.
    """
    derivative_range = 2 * sides - 1
    counts = [0] * derivative_range
    for a, b in zip(values_1indexed, values_1indexed[1:]):
        counts[b - a + sides - 1] += 1
    derivatives = len(values_1indexed) - 1
    entropy = 0.0
    for count in counts:
        if count:
            p = count / derivatives
            entropy -= p * math.log2(p)
    max_entropy = math.log2(derivative_range)
    normalized = (max_entropy - entropy) / max_entropy * 100 if max_entropy > 0 else 0
    return normalized > 30


def pattern_detected(kind, values):
    """Whether consecutive rolls look like an arithmetic run rather than random.

    Mirrors seedtool_dice_pattern_detected: below 10 rolls this always reports
    False, since a handful of rolls can look patterned by chance alone.
    """
    sides = {"d6": 6, "d20": 20, "coin": 2, "cards-replace": 52}[kind]
    if any(v < 1 or v > sides for v in values):
        raise ValueError(f"{kind} values must be 1..{sides}")
    if len(values) < 10:
        return False
    return derivative_pattern_detected(values, sides)


def coin_faces(entries):
    """0/1 per entry, remapped to a 1/2 face: mirrors the local remap
    main/seedtool_app.c's entropy_quality does before calling
    seedtool_dice_entropy_bits/seedtool_dice_pattern_detected, since those
    expect every source they cover to be 1-indexed like D6/D20 already are.
    """
    bits = [{"h": "1", "heads": "1", "1": "1", "t": "0", "tails": "0", "0": "0"}.get(x.lower()) for x in entries]
    if any(x is None for x in bits):
        raise ValueError("coins must be H/T or 1/0")
    return [int(b) + 1 for b in bits]


def card_values(entries):
    """0..51 per entry (rank + suit*13), the same encoding transcript() and
    seedtool_transcript use. Does not itself check for a repeat - card
    draws are only ever passed here after transcript() has already applied
    whichever distinctness rule its kind calls for."""
    ranks, suits = "A23456789TJQK", "CDHS"
    vals = [x.upper() for x in entries]
    allowed = {r + s for s in suits for r in ranks}
    if any(x not in allowed for x in vals):
        raise ValueError("cards must be rank+suit codes (AC..KS)")
    return [suits.index(v[1]) * 13 + ranks.index(v[0]) for v in vals]


def card_entropy_bits(drawn_count):
    """Mirrors seedtool_card_entropy_bits: cards are drawn without
    replacement, so unlike a die's distribution this is exact rather than
    estimated - log2 of how many equally likely ordered draws of that length
    exist, not a value estimated from a sample.
    """
    if drawn_count > 52:
        raise ValueError("cannot draw more than 52 distinct cards")
    return int(sum(math.log2(52 - i) for i in range(drawn_count)))


def card_pattern_detected(values):
    """Mirrors seedtool_card_pattern_detected: rank alone (ignoring suit),
    re-indexed 1..13, through the same derivative-entropy check dice uses.
    """
    if len(values) < 10:
        return False
    return derivative_pattern_detected([v % 13 + 1 for v in values], 13)


def inv(x):
    return pow(x, P - 2, P)


def add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0]:
        if (a[1] + b[1]) % P == 0:
            return None
        slope = 3 * a[0] * a[0] * inv(2 * a[1] % P) % P
    else:
        slope = (b[1] - a[1]) * inv((b[0] - a[0]) % P) % P
    x = (slope * slope - a[0] - b[0]) % P
    return x, (slope * (a[0] - x) - a[1]) % P


def mul(k, point=G):
    result = None
    while k:
        if k & 1:
            result = add(result, point)
        point = add(point, point)
        k >>= 1
    return result


def pubkey(secret):
    x, y = mul(secret)
    return bytes([2 | (y & 1)]) + x.to_bytes(32, "big")


def hash160(data):
    return hashlib.new("ripemd160", hashlib.sha256(data).digest()).digest()


# A node is (secret, chaincode, depth, parent fingerprint, child number); the
# last three carry no key material and exist only to serialise an xpub.
def master(mnemonic, passphrase):
    seed = hashlib.pbkdf2_hmac("sha512", mnemonic.encode(), ("mnemonic" + passphrase).encode(), 2048)
    digest = hmac.new(b"Bitcoin seed", seed, hashlib.sha512).digest()
    secret = int.from_bytes(digest[:32], "big")
    if not 0 < secret < N:
        raise ValueError("invalid BIP32 master key")
    return secret, digest[32:], 0, bytes(4), 0


def child_private(node, index):
    secret, chain, depth, _, _ = node
    data = b"\0" + secret.to_bytes(32, "big") if index & H else pubkey(secret)
    digest = hmac.new(chain, data + index.to_bytes(4, "big"), hashlib.sha512).digest()
    child = (secret + int.from_bytes(digest[:32], "big")) % N
    if not child:
        raise ValueError("invalid BIP32 child key")
    return child, digest[32:], depth + 1, hash160(pubkey(secret))[:4], index


def derive(node, path):
    for part in path:
        node = child_private(node, part)
    return node


def base58check(payload):
    data = payload + hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    number = int.from_bytes(data, "big")
    encoded = ""
    while number:
        number, remainder = divmod(number, 58)
        encoded = BASE58[remainder] + encoded
    return "1" * (len(data) - len(data.lstrip(b"\0"))) + encoded


def xpub(node, version=XPUB_VERSION):
    secret, chain, depth, parent, index = node
    return base58check(version + bytes([depth]) + parent + index.to_bytes(4, "big") + chain + pubkey(secret))


def zpub(node):
    """The same account key as xpub(), with SLIP-132's native-segwit version bytes.

    libwally has no notion of SLIP-132: this is the whole 78-byte payload
    unchanged except for the four leading version bytes, which is also how the
    device itself builds a zpub.
    """
    return xpub(node, ZPUB_VERSION)


def polymod(values):
    chk = 1
    gen = (0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3)
    for value in values:
        top, chk = chk >> 25, ((chk & 0x1FFFFFF) << 5) ^ value
        for i in range(5):
            chk ^= gen[i] if (top >> i) & 1 else 0
    return chk


def convertbits(data, frombits=8, tobits=5):
    acc = bits = 0
    result = []
    for value in data:
        acc = (acc << frombits) | value
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            result.append((acc >> bits) & ((1 << tobits) - 1))
    if bits:
        result.append((acc << (tobits - bits)) & ((1 << tobits) - 1))
    return result


def segwit(program, version):
    hrp = "bc"
    values = [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp] + [version] + convertbits(program)
    constant = 1 if version == 0 else 0x2BC830A3
    mod = polymod(values + [0] * 6) ^ constant
    payload = [version] + convertbits(program) + [(mod >> 5 * (5 - i)) & 31 for i in range(6)]
    return hrp + "1" + "".join(CHARSET[x] for x in payload)


def tagged_hash(tag, data):
    th = hashlib.sha256(tag).digest()
    return hashlib.sha256(th + th + data).digest()


def addresses(mnemonic, passphrase, index, account_index=0):
    root = master(mnemonic, passphrase)
    result, accounts = {}, {}
    for purpose in (84, 86):
        account = derive(root, (purpose | H, 0 | H, account_index | H))
        accounts[purpose] = xpub(account)
        if purpose == 84:
            accounts["84z"] = zpub(account)
        node = derive(account, (0, index))
        if purpose == 84:
            result[purpose] = segwit(hash160(pubkey(node[0])), 0)
        else:
            x, y = mul(node[0])
            internal = N - node[0] if y & 1 else node[0]
            tweak = int.from_bytes(tagged_hash(b"TapTweak", x.to_bytes(32, "big")), "big")
            out_x, _ = mul((internal + tweak) % N)
            result[purpose] = segwit(out_x.to_bytes(32, "big"), 1)
    return hash160(pubkey(root[0]))[:4].hex(), result, accounts


def generate(args):
    counts = {
        "d6": (50, 99),
        "d20": (30, 60),
        "coin": (128, 256),
        "cards": (25, None),
        # Without replacement, a single deck tops out around 225.6 bits
        # (log2(52!)) - short of 256 even drawing every card - so 24 words
        # instead returns the card to the deck and reshuffles before every
        # draw, needing more draws (see shannon_bits/pattern_detected,
        # which grade this the same way as any other die).
        "cards-replace": (None, 48),
    }
    count = counts[args.source][0 if args.words == 12 else 1]
    if count is None:
        supported = 24 if args.words == 12 else 12
        raise ValueError(f"{args.source} mode supports {supported} words only")
    text = transcript(args.source, args.entries, count)
    digest = hashlib.sha256(text.encode("ascii")).digest()
    mnemonic = mnemonic_from_entropy(digest[:16] if args.words == 12 else digest)
    print("transcript:", text)
    print("sha256:    ", digest.hex())
    print("mnemonic:  ", mnemonic)
    if args.stats:
        min_bits = 128 if args.words == 12 else 256
        if args.source == "cards":
            values = card_values(args.entries)
            print(f"bits:       {card_entropy_bits(len(values))} bits (min {min_bits})")
            print("pattern:   ", "detected" if card_pattern_detected(values) else "not detected")
        else:
            if args.source == "cards-replace":
                values = [v + 1 for v in card_values(args.entries)]
            elif args.source == "coin":
                values = coin_faces(args.entries)
            else:
                values = [int(x) for x in args.entries]
            print(f"shannon:    {shannon_bits(args.source, values)} bits (min {min_bits})")
            print("pattern:   ", "detected" if pattern_detected(args.source, values) else "not detected")


def inspect(args):
    mnemonic_entropy(args.mnemonic)
    fingerprint, addrs, accounts = addresses(args.mnemonic, args.passphrase, args.index, args.account)
    print("checksum:   valid")
    print("word numbers:", " ".join(str(n) for n in word_numbers(args.mnemonic)))
    print("compact seedqr (hex):", compact_seedqr_payload(args.mnemonic).hex())
    print("fingerprint:", fingerprint)
    for purpose in (84, 86):
        print(f"[{fingerprint}/{purpose}'/0'/{args.account}']: {accounts[purpose]}")
    print(f"[{fingerprint}/84'/0'/{args.account}'] (zpub): {accounts['84z']}")
    for purpose in (84, 86):
        print(f"qr {purpose}: {account_qr_payload(fingerprint, purpose, accounts[purpose], args.account)}")
    for purpose in (84, 86):
        print(f"m/{purpose}'/0'/{args.account}'/0/{args.index}: {addrs[purpose]}")
    for purpose in (84, 86):
        print(f"descriptor {purpose}: {descriptor(fingerprint, purpose, accounts[purpose], args.account)}")


def complete(args):
    wl = words()
    prefix = args.prefix.split()
    needed = 7 if len(prefix) == 11 else 3 if len(prefix) == 23 else 0
    if not needed or len(args.bits) != needed or any(x not in "01" for x in args.bits):
        raise ValueError("11 words require 7 bits; 23 words require 3 bits")
    try:
        packed = 0
        for word in prefix:
            packed = (packed << 11) | wl.index(word)
    except ValueError as exc:
        raise ValueError("prefix contains a non-BIP39 word") from exc
    entropy = ((packed << needed) | int(args.bits, 2)).to_bytes(16 if needed == 7 else 32, "big")
    print(mnemonic_from_entropy(entropy))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(required=True)
    gen = sub.add_parser("generate")
    gen.add_argument("source", choices=("d6", "d20", "coin", "cards", "cards-replace"))
    gen.add_argument("entries", nargs="+")
    gen.add_argument("--words", type=int, choices=(12, 24), default=12)
    gen.add_argument("--stats", action="store_true", help="print Shannon's entropy and pattern check (d6/d20 only)")
    gen.set_defaults(func=generate)
    check = sub.add_parser("inspect")
    check.add_argument("mnemonic")
    check.add_argument("--passphrase", default="")
    check.add_argument("--index", type=int, choices=range(100), default=0)
    check.add_argument("--account", type=int, choices=range(1000), default=0)
    check.set_defaults(func=inspect)
    comp = sub.add_parser("complete")
    comp.add_argument("prefix")
    comp.add_argument("bits")
    comp.set_defaults(func=complete)
    args = parser.parse_args()
    try:
        args.func(args)
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
