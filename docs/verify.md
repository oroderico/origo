# Verifying Origo

Checking the device's output, its QR encoder and its generated assets against independent implementations.

## Independent verification

`tools/origo_verify.py` is dependency-free and independently implements
BIP39, BIP32, BIP44, BIP49, BIP84, BIP86, Base58Check and Bech32/Bech32m.
Examples:

```sh
python3 tools/origo_verify.py generate d20 --words 12 1 2 3 ...
python3 tools/origo_verify.py complete "abandon ... abandon" 0000000
python3 tools/origo_verify.py coin-words 0110011001101100110011...   # 128 flips
python3 tools/origo_verify.py inspect "abandon ... about" --index 0
python3 tools/origo_verify.py inspect "abandon ... about" --index 0 --change
```

`coin-words` reproduces the Flip-each-word method from the flips alone,
printing each group of eleven with the word it names before the finished
mnemonic — so the device's screens can be checked one at a time, or the whole
run at once. It is the same arithmetic `complete` performs, entered from the
coins rather than from words already converted by hand.

`inspect` prints the checksum verdict, one-based word numbers, the Compact
SeedQR payload, master fingerprint, both account xpubs, the exact payload of
both account key QR codes, both addresses and both output descriptors, so
every value the device can display has an independent second implementation to
be compared against. `--change` derives the change branch instead of receive,
so an address read off the device's Change list can be checked against the
same second implementation the receive one is. It also prints each descriptor's
pre-BIP389 receive-only form, which the device itself does not show — that
line is byte for byte what the device exported before the multipath change,
and it is what to hand a wallet that rejects `<0;1>`.

Do not type a real mnemonic or passphrase into a network-connected computer.
Boot a trusted offline environment, verify this repository and tool first, and
compare the transcript, full hash, fingerprint, xpubs, and addresses.

### The QR encoder

Every QR the device draws, the Compact SeedQR among them, comes from
`main/qrcode.c`: Richard Moore's port of Project Nayuki's QR Code generator,
as Blockstream Jade carries it. Origo's copy starts from Jade's at commit
`4765740e3aad`, and the difference is the review:

```sh
J=https://raw.githubusercontent.com/Blockstream/Jade/4765740e3aadc096800f42a3cccbe2d5a54b582f/main
curl -fsSL $J/qrcode.c | diff - main/qrcode.c
curl -fsSL $J/qrcode.h | diff - main/qrcode.h
```

What it shows: Jade's icon helpers and their includes removed; the per-version
tables for versions 5 and 6 added to the `LOCK_VERSION` builds, which Origo
does not use; `qrcode_versionForBytes`, `qrcode_versionForAlphanumeric` and
`qrcode_versionForText`, which pick the smallest version a payload fits, since
the encoder itself never checks that it does; and `qr_wipe`, which zeroes the
encoder's codeword and error-correction buffers before it returns, as they
hold the payload. Nothing in the encoding itself is changed.

That last claim is tested rather than asserted. `tests/qr_vectors.h` holds
the full matrix of 25 payloads - Compact SeedQRs, the four address types, a
key origin and zpub, a BBQr frame, and the largest byte and alphanumeric
payload of each version from 1 to 6 - and `tests/test_qrcode.c` requires the
encoder to reproduce each one bit for bit, at the version the firmware would
pick. `tools/make_qr_vectors.py` made them: each matrix is Nayuki's own
qrcodegen 1.8.0 encoding of the payload, decoded back to it by zxing-cpp
3.1.1, an unrelated decoder, and the tool refuses to write a vector where
Origo's matrix differs from qrcodegen's. The one thing taken from Origo is the
mask: its penalty scoring is older than qrcodegen's and sometimes picks
another of the eight, which a decoder reads from the code's format bits.

Raw bytes that happen to be all digits, or all characters of QR's
alphanumeric set, are encoded in that denser mode rather than byte mode, as
the original library does; a decoder returns the same bytes. Two of the
vectors are such Compact SeedQRs. Random entropy lands there with probability
under 10^-12.

## The splash screen and its artwork

The opening screen shows the logo as drawn — mark, wordmark and tagline in one
picture — for a couple of seconds and then gives way to the menu on its own.
Presses are discarded while it is up: it is where the user is still learning that
both buttons together mean select, and a screen that offers no choice must not
turn a stray press into one.

That artwork is the only picture in the firmware: a 102x124 array of palette
indices at two pixels per byte, about 6.2 KiB, written straight into the
framebuffer by indexing a palette of nine colours. It is deliberately not
compressed. Deflate would take it to well under half, but the decompressor is
one of the components the audit rejects by name, and trading an audited absence
for a few kilobytes is a bad exchange.

The logo is pixel art, and its source is its cells, not a picture of them:
`assets/logo.txt` holds the tree, the wordmark and the tagline as grids of
colour letters, each drawn at its own whole number of display pixels per cell -
3 for the tree, 2 for the wordmark, 1 for the tagline - so no cell is split
across pixels or blended into its neighbour. `tools/make_logo.py` turns those
cells into `main/seedtool_logo.c` and into `assets/logo.png`, the same picture
at eight times the size; edit the cells and run it. It needs only the standard
library.

### Fonts

Text is set in Ubuntu at 16 pixels: screen titles in Regular and what the
screen holds - rows, words, numbers, addresses, keys - in Medium, as Jade sets
labels lighter than values. Both are one bit per pixel, like the artwork: no
glyph edge is blended, and the renderer draws a font by setting pixels, with
no coverage to mix. `tools/make_font.py` generates `main/fonts/UbuntuRegular16.c`
and `main/fonts/UbuntuMedium16.c` from the two TTFs whose SHA256s it and
`THIRD_PARTY_NOTICES.md` record. It is the one tool that needs more than the
standard library: Pillow, whose FreeType does the hinting. Hinting changes
between FreeType releases, so the arrays are reproduced exactly with the
versions the tool names (Pillow 11.1.0, FreeType 2.13.2), and it warns under
others. To check the committed fonts:

```sh
python3 -m venv /tmp/fonts && /tmp/fonts/bin/pip install pillow==11.1.0
mkdir /tmp/ttf && for f in Ubuntu-Regular.ttf Ubuntu-Medium.ttf; do
  curl -fsSLo /tmp/ttf/$f https://github.com/google/fonts/raw/main/ufl/ubuntu/$f
done
/tmp/fonts/bin/python tools/make_font.py /tmp/ttf && git diff --exit-code main/fonts
```

The tool refuses a TTF whose SHA256 differs, so an upstream change to the
files shows up as that refusal rather than as different glyphs.
