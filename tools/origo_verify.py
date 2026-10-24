#!/usr/bin/env python3
"""Independent, dependency-free verifier for Origo outputs.

Do not type a real mnemonic or passphrase into a network-connected computer.
Run this script from a trusted, offline environment.
"""

import argparse
import hashlib
import hmac
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
YPUB_VERSION = b"\x04\x9d\x7c\xb2"  # SLIP-132, nested segwit (BIP49) only
# Every script type the device derives, in its own order: legacy, nested
# segwit, native segwit, taproot.
PURPOSES = (44, 49, 84, 86)
# Each type's output descriptor wrapper (BIP381-386).
SCRIPT_WRAP = {44: ("pkh(", ")"), 49: ("sh(wpkh(", "))"), 84: ("wpkh(", ")"), 86: ("tr(", ")")}
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


def descriptor(fingerprint, purpose, xpub, account=0, multipath=True):
    """The full wpkh()/tr() output descriptor Origo's own Descriptor export
    shows, checksum included - script fragment matches the address type,
    always plain xpub (BIP380 has no notion of SLIP-132's zpub). The chain
    step is BIP389's multipath <0;1>, so one descriptor carries both the
    receive and the change branch, exactly as the firmware writes it.

    multipath=False emits the older receive-only /0/* form instead, for a
    wallet predating BIP389 that rejects <0;1> rather than reading it. The
    firmware has no such setting - it is the one descriptor shape the device
    shows - so this exists to let a reader produce the fallback offline
    rather than hand-editing a string whose checksum then no longer matches."""
    opening, closing = SCRIPT_WRAP[purpose]
    chain = "<0;1>" if multipath else "0"
    body = f"{opening}[{fingerprint}/{purpose}'/0'/{account}']{xpub}/{chain}/*{closing}"
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
    tweak = int.from_bytes(digest[:32], "big")
    child = (secret + tweak) % N
    # BIP32 invalidates a child on either condition, and the device rejects
    # both: libwally reaches secp256k1_ec_seckey_tweak_add, which fails on the
    # scalar overflow as well as on a zero result (see the "seckey_tweak_add
    # checks both conditions" note in libwally's bip32.c). Checking only for
    # zero here meant that on a tweak at or above the curve order the device
    # would refuse to derive while this tool quietly returned a key - the two
    # disagreeing is the one failure a verifier must not have. Roughly a
    # 2**-128 event per derivation, so this is about the implementations
    # matching, not about anyone hitting it.
    if tweak >= N or not child:
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


def ypub(node):
    """The same account key with SLIP-132's nested-segwit version bytes."""
    return xpub(node, YPUB_VERSION)


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


def addresses(mnemonic, passphrase, index, account_index=0, chain=0):
    root = master(mnemonic, passphrase)
    result, accounts = {}, {}
    for purpose in PURPOSES:
        account = derive(root, (purpose | H, 0 | H, account_index | H))
        accounts[purpose] = xpub(account)
        if purpose == 84:
            accounts["84z"] = zpub(account)
        if purpose == 49:
            accounts["49y"] = ypub(account)
        node = derive(account, (chain, index))
        if purpose == 44:
            result[purpose] = base58check(b"\x00" + hash160(pubkey(node[0])))
        elif purpose == 49:
            redeem = b"\x00\x14" + hash160(pubkey(node[0]))
            result[purpose] = base58check(b"\x05" + hash160(redeem))
        elif purpose == 84:
            result[purpose] = segwit(hash160(pubkey(node[0])), 0)
        else:
            x, y = mul(node[0])
            internal = N - node[0] if y & 1 else node[0]
            tweak = int.from_bytes(tagged_hash(b"TapTweak", x.to_bytes(32, "big")), "big")
            out_x, _ = mul((internal + tweak) % N)
            result[purpose] = segwit(out_x.to_bytes(32, "big"), 1)
    return hash160(pubkey(root[0]))[:4].hex(), result, accounts


def inspect(args):
    mnemonic_entropy(args.mnemonic)
    chain = 1 if args.change else 0
    fingerprint, addrs, accounts = addresses(args.mnemonic, args.passphrase, args.index, args.account, chain)
    print("checksum:   valid")
    print("word numbers:", " ".join(str(n) for n in word_numbers(args.mnemonic)))
    print("compact seedqr (hex):", compact_seedqr_payload(args.mnemonic).hex())
    print("fingerprint:", fingerprint)
    for purpose in PURPOSES:
        print(f"[{fingerprint}/{purpose}'/0'/{args.account}']: {accounts[purpose]}")
    print(f"[{fingerprint}/49'/0'/{args.account}'] (ypub): {accounts['49y']}")
    print(f"[{fingerprint}/84'/0'/{args.account}'] (zpub): {accounts['84z']}")
    for purpose in PURPOSES:
        print(f"qr {purpose}: {account_qr_payload(fingerprint, purpose, accounts[purpose], args.account)}")
    for purpose in PURPOSES:
        print(f"m/{purpose}'/0'/{args.account}'/{chain}/{args.index}: {addrs[purpose]}")
    for purpose in PURPOSES:
        print(f"descriptor {purpose}: {descriptor(fingerprint, purpose, accounts[purpose], args.account)}")
    # The pre-BIP389 fallback, for a wallet that rejects the multipath form.
    # Not what the device shows; printed so it never has to be hand-edited.
    for purpose in PURPOSES:
        receive_only = descriptor(fingerprint, purpose, accounts[purpose], args.account, multipath=False)
        print(f"descriptor {purpose} (receive-only): {receive_only}")


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


def coin_words(args):
    """The device's Flip each word method, from the flips alone.

    Eleven flips name one word: the wordlist is exactly 2048 long, so an
    eleven-bit index is the whole of it, most significant bit first. Eleven
    such words are 121 bits and the entropy needs 128, so seven loose flips
    finish it; 24 words is twenty-three words and three loose flips for 256.

    `complete` already does the arithmetic below the words - this exists so
    the whole thing can be checked from what the reader actually wrote down,
    which is a string of heads and tails, without converting each group of
    eleven against a printed wordlist by hand first. Doing it by hand is the
    point of the method; not being *required* to is the point of this.
    """
    flips = "".join(args.flips.split())
    if any(character not in "01" for character in flips):
        raise ValueError("flips are 1 for heads and 0 for tails")
    word_count = 11 if args.words == 12 else 23
    needed = word_count * 11 + (7 if args.words == 12 else 3)
    if len(flips) != needed:
        raise ValueError(f"{args.words} words need {needed} flips, got {len(flips)}")
    wl = words()
    indices = [int(flips[i * 11 : (i + 1) * 11], 2) for i in range(word_count)]
    for position, index in enumerate(indices, start=1):
        print(f"word {position:2}: {flips[(position - 1) * 11 : position * 11]}  {index + 1:4}  {wl[index]}")
    tail = flips[word_count * 11 :]
    entropy = ((int("".join(f"{i:011b}" for i in indices), 2) << len(tail)) | int(tail, 2)).to_bytes(
        16 if args.words == 12 else 32, "big")
    print(f"loose bits: {tail}")
    print("mnemonic:  ", mnemonic_from_entropy(entropy))


# The faces that count, the bits each gives, and the rolls a word takes.
DICE = {"d6": (4, 2, 6), "d20": (16, 4, 3)}


def dice(args):
    """The device's D6 and D20 methods, from the rolls alone.

    Only as many faces count as are a power of two - 1 to 4 on a D6, 1 to 16
    on a D20 - and the rest are rolled again, so every counted face is equally
    likely. Face f is the bits of f - 1, most significant first. A word is
    eleven bits, which 2 and 4 do not divide, so its last roll keeps only its
    first bits: a D6 word is five rolls of 2 bits and one of 1, a D20 word two
    of 4 bits and one of 3. Then the 7 or 3 loose coin bits finish the
    mnemonic, as `coin-words` does.
    """
    faces, bits, per_word = DICE[args.die]
    rolls = [int(roll) for roll in " ".join(args.rolls).replace(",", " ").split()]
    if len(rolls) not in (11 * per_word, 23 * per_word):
        raise ValueError(f"{args.die} needs {11 * per_word} or {23 * per_word} rolls, got {len(rolls)}")
    if any(not 1 <= roll <= faces for roll in rolls):
        raise ValueError(f"only faces 1 to {faces} count on a {args.die}; roll the others again")
    word_count = len(rolls) // per_word
    needed = 7 if word_count == 11 else 3
    if len(args.bits) != needed or any(character not in "01" for character in args.bits):
        raise ValueError(f"{word_count} words need {needed} loose bits, 1 for heads and 0 for tails")
    wl = words()
    last_bits = 11 - bits * (per_word - 1)
    indices = []
    for position in range(word_count):
        word_rolls = rolls[position * per_word : (position + 1) * per_word]
        index = 0
        for roll in word_rolls[:-1]:
            index = (index << bits) | (roll - 1)
        index = (index << last_bits) | ((word_rolls[-1] - 1) >> (bits - last_bits))
        indices.append(index)
        shown = ", ".join(str(roll) for roll in word_rolls)
        print(f"word {position + 1:2}: {shown}  {index + 1:04}  {wl[index]}")
    entropy = ((int("".join(f"{i:011b}" for i in indices), 2) << needed) | int(args.bits, 2)).to_bytes(
        16 if needed == 7 else 32, "big")
    print(f"loose bits: {args.bits}")
    print("mnemonic:  ", mnemonic_from_entropy(entropy))


def tickets(args):
    """The device's SeedPicker tickets method, from the draws alone.

    A ticket T1..T342, put back after each draw, and a six-sided die name word
    number (ticket - 1) * 6 + die: 2052 pairs for 2048 words, the four past
    2048 (T342 with 3 to 6) drawn again from the ticket. Eleven or twenty-three
    words, then the 7 or 3 loose coin bits, finish exactly as `coin-words`
    does. Three tails reproduce SeedPicker's own last-word calculator, which
    always picks the first valid word alphabetically.
    """
    if len(args.draws) not in (11, 23):
        raise ValueError(f"11 or 23 draws make a mnemonic, got {len(args.draws)}")
    needed = 7 if len(args.draws) == 11 else 3
    if len(args.bits) != needed or any(character not in "01" for character in args.bits):
        raise ValueError(f"{len(args.draws)} draws need {needed} loose bits, 1 for heads and 0 for tails")
    wl = words()
    indices = []
    for position, draw in enumerate(args.draws, start=1):
        try:
            ticket, die = (int(part) for part in draw.split(":"))
        except ValueError as exc:
            raise ValueError(f"draw {position} is {draw!r}, not TICKET:DIE") from exc
        if not 1 <= ticket <= 342 or not 1 <= die <= 6:
            raise ValueError(f"draw {position}: tickets run T1 to T342 and the die 1 to 6")
        number = (ticket - 1) * 6 + die
        if number > 2048:
            raise ValueError(f"draw {position}: T{ticket} + {die} is past 2048; draw a new ticket")
        indices.append(number - 1)
        print(f"word {position:2}: T{ticket} + die {die}  {number:04}  {wl[number - 1]}")
    entropy = ((int("".join(f"{i:011b}" for i in indices), 2) << needed) | int(args.bits, 2)).to_bytes(
        16 if needed == 7 else 32, "big")
    print(f"loose bits: {args.bits}")
    print("mnemonic:  ", mnemonic_from_entropy(entropy))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(required=True)
    check = sub.add_parser("inspect")
    check.add_argument("mnemonic")
    check.add_argument("--passphrase", default="")
    check.add_argument("--index", type=int, choices=range(10000), default=0, metavar="0-9999")
    check.add_argument("--account", type=int, choices=range(1000), default=0)
    check.add_argument("--change", action="store_true", help="derive the change branch (chain 1) instead of receive")
    check.set_defaults(func=inspect)
    comp = sub.add_parser("complete")
    comp.add_argument("prefix")
    comp.add_argument("bits")
    comp.set_defaults(func=complete)
    coin = sub.add_parser("coin-words")
    coin.add_argument("flips", help="128 or 256 flips, 1 for heads and 0 for tails; spaces ignored")
    coin.add_argument("--words", type=int, choices=(12, 24), default=12)
    coin.set_defaults(func=coin_words)
    die = sub.add_parser("dice")
    die.add_argument("die", choices=sorted(DICE))
    die.add_argument("rolls", nargs="+", help="every roll in order, spaces or commas between them")
    die.add_argument("--bits", required=True, help="the 7 or 3 loose coin bits, 1 for heads and 0 for tails")
    die.set_defaults(func=dice)
    tick = sub.add_parser("tickets")
    tick.add_argument("draws", nargs="+", help="11 or 23 draws as TICKET:DIE, e.g. 57:4")
    tick.add_argument("--bits", required=True, help="the 7 or 3 loose coin bits, 1 for heads and 0 for tails")
    tick.set_defaults(func=tickets)
    args = parser.parse_args()
    try:
        args.func(args)
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    main()
