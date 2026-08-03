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
    ranks, suits = "A23456789TJQK", "CDHS"
    vals = [x.upper() for x in entries]
    allowed = {r + s for s in suits for r in ranks}
    if len(set(vals)) != len(vals) or any(x not in allowed for x in vals):
        raise ValueError("cards must be distinct rank+suit codes (AC..KS)")
    return "cards-v1:" + "".join(vals)


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


def xpub(node):
    secret, chain, depth, parent, index = node
    return base58check(
        XPUB_VERSION + bytes([depth]) + parent + index.to_bytes(4, "big") + chain + pubkey(secret)
    )


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


def addresses(mnemonic, passphrase, index):
    root = master(mnemonic, passphrase)
    result, accounts = {}, {}
    for purpose in (84, 86):
        account = derive(root, (purpose | H, 0 | H, 0 | H))
        accounts[purpose] = xpub(account)
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
    counts = {"d6": (50, 99), "d20": (30, 60), "coin": (128, 256), "cards": (25, None)}
    count = counts[args.source][0 if args.words == 12 else 1]
    if count is None:
        raise ValueError("cards mode supports 12 words only")
    text = transcript(args.source, args.entries, count)
    digest = hashlib.sha256(text.encode("ascii")).digest()
    mnemonic = mnemonic_from_entropy(digest[:16] if args.words == 12 else digest)
    print("transcript:", text)
    print("sha256:    ", digest.hex())
    print("mnemonic:  ", mnemonic)


def inspect(args):
    mnemonic_entropy(args.mnemonic)
    fingerprint, addrs, accounts = addresses(args.mnemonic, args.passphrase, args.index)
    print("checksum:   valid")
    print("fingerprint:", fingerprint)
    for purpose in (84, 86):
        print(f"[{fingerprint}/{purpose}'/0'/0']: {accounts[purpose]}")
    for purpose in (84, 86):
        print(f"m/{purpose}'/0'/0'/0/{args.index}: {addrs[purpose]}")


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
    gen.add_argument("source", choices=("d6", "d20", "coin", "cards"))
    gen.add_argument("entries", nargs="+")
    gen.add_argument("--words", type=int, choices=(12, 24), default=12)
    gen.set_defaults(func=generate)
    check = sub.add_parser("inspect")
    check.add_argument("mnemonic")
    check.add_argument("--passphrase", default="")
    check.add_argument("--index", type=int, choices=range(100), default=0)
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
