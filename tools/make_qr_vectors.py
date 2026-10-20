#!/usr/bin/env python3
"""Generate tests/qr_vectors.h: QR matrices from an independent encoder,
checked by an independent decoder, that main/qrcode.c must reproduce bit for
bit.

main/qrcode.c is a modified copy of Richard Moore's port of Project Nayuki's
QR Code generator (see THIRD_PARTY_NOTICES.md). For every payload below this
tool:

1. encodes it with main/qrcode.c, compiled here with `cc`, choosing the
   version as the firmware does (qrcode_versionForText for text,
   qrcode_versionForBytes for raw bytes), and notes the mask it picked;
2. encodes it again with Nayuki's own qrcodegen at that version, error
   correction level L, the same mode and that mask;
3. decodes qrcodegen's matrix with zxing-cpp, an unrelated decoder, and
   requires the original payload back;
4. requires the two matrices to be identical, and writes them out.

tests/test_qrcode.c then checks, on every CI run, that main/qrcode.c still
produces exactly these matrices. The mask is taken from main/qrcode.c because
its penalty scoring is older than qrcodegen 1.8's and sometimes picks another
of the eight; any mask is valid, since a decoder reads it from the code's
format bits. Authoring tool, run by hand when the payloads change:

    pip install qrcodegen==1.8.0 zxing-cpp==3.1.1 numpy
    python3 tools/make_qr_vectors.py
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

try:
    import numpy
    import zxingcpp
    from qrcodegen import QrCode, QrSegment
except ImportError:  # pragma: no cover - authoring-only dependency
    sys.exit("this tool needs qrcodegen==1.8.0, zxing-cpp==3.1.1 and numpy")

ROOT = Path(__file__).parents[1]
OUTPUT = ROOT / "tests/qr_vectors.h"
# QR_VERSION in main/seedtool_render.c.
MAX_VERSION = 6
ALPHANUMERIC = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"

# (name, payload, text?). Text goes through qrcode_initText, as addresses,
# keys and BBQr frames do; bytes through qrcode_initBytes, as Compact SeedQR
# does.
PAYLOADS = [
    ("compact_seedqr_12_zero", bytes(16), False),
    ("compact_seedqr_12_7f", b"\x7f" * 16, False),
    ("compact_seedqr_12_80", b"\x80" * 16, False),
    ("compact_seedqr_24_zero", bytes(32), False),
    ("compact_seedqr_24_ff", b"\xff" * 32, False),
    # Entropy whose bytes all happen to be digits, or all alphanumeric
    # characters, is encoded in that denser mode rather than byte mode. It
    # decodes to the same bytes either way.
    ("compact_seedqr_12_digits", b"0123456789012345", False),
    ("compact_seedqr_12_alphanumeric", b"ABCDEFGHIJKLMNOP", False),
    ("address_p2wpkh", b"bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu", True),
    ("address_p2tr", b"bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr", True),
    ("address_p2pkh", b"1LqBGSKuX5yYUonjxT5qGfpUsXKYYWeabA", True),
    ("address_p2sh_p2wpkh", b"37VucYSaXLCAsxYyAPfbSi9eh4iEcbShgf", True),
    (
        "account_zpub_with_origin",
        b"[73c5da0a/84'/0'/0']zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs",
        True,
    ),
    # A BBQr frame at the largest version frames are drawn at (3: 77
    # alphanumeric characters).
    ("bbqr_frame_v3", b"B$2U0201" + b"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567" * 2 + b"ABCDE", True),
]
# The largest payload each version holds, in byte mode and as alphanumeric
# text, so every version and both length-field sizes are covered.
for version, length in enumerate((17, 32, 53, 78, 106, 134), start=1):
    PAYLOADS.append((f"bytes_v{version}_full", bytes((i * 37 + version) & 0xFF for i in range(length)), False))
for version, length in enumerate((25, 47, 77, 114, 154, 195), start=1):
    PAYLOADS.append((f"alphanumeric_v{version}_full", "".join(ALPHANUMERIC[(i * 7 + version) % 45] for i in range(length)).encode(), True))

DUMPER = r"""
#include "qrcode.h"
#include <stdio.h>
#include <string.h>
/* Lines of "T <hex>" or "B <hex>" in; "<version> <mask> <module buffer hex>" out. */
int main(void)
{
    char kind, hex[1024];
    while (scanf(" %c %1023s", &kind, hex) == 2) {
        uint8_t data[512];
        const size_t length = strlen(hex) / 2;
        for (size_t i = 0; i < length; ++i) {
            (void)sscanf(hex + 2 * i, "%2hhx", &data[i]);
        }
        data[length] = 0;
        const uint8_t version = kind == 'T' ? qrcode_versionForText(ECC_LOW, (const char*)data, MAX_VERSION)
                                            : qrcode_versionForBytes(ECC_LOW, (uint16_t)length, MAX_VERSION);
        uint8_t modules[qrcode_getBufferSize(MAX_VERSION)];
        QRCode qr;
        const int8_t failed = !version ? 1
            : kind == 'T' ? qrcode_initText(&qr, modules, version, ECC_LOW, (const char*)data)
                          : qrcode_initBytes(&qr, modules, version, ECC_LOW, data, (uint16_t)length);
        if (failed) {
            puts("0 0 -");
            continue;
        }
        printf("%u %u ", qr.version, qr.mask);
        for (uint16_t i = 0; i < qrcode_getBufferSize(version); ++i) {
            printf("%02x", modules[i]);
        }
        putchar('\n');
    }
    return 0;
}
""".replace("MAX_VERSION", str(MAX_VERSION))


def encode_ours(payloads):
    with tempfile.TemporaryDirectory() as scratch:
        source, binary = Path(scratch) / "dump.c", Path(scratch) / "dump"
        source.write_text(DUMPER)
        subprocess.run(
            [os.environ.get("CC", "cc"), "-O1", "-I", str(ROOT / "main"), "-o", str(binary), str(source),
             str(ROOT / "main/qrcode.c")],
            check=True,
        )
        lines = "".join(f"{'T' if text else 'B'} {payload.hex()}\n" for _, payload, text in payloads)
        out = subprocess.run([str(binary)], input=lines, capture_output=True, text=True, check=True).stdout
    return [line.split() for line in out.splitlines()]


def segment(payload):
    """The mode main/qrcode.c's encodeDataCodewords picks: numeric, then
    alphanumeric, then bytes."""
    text = payload.decode("latin-1")
    if all(c in "0123456789" for c in text):
        return QrSegment.make_numeric(text)
    if all(c in ALPHANUMERIC for c in text):
        return QrSegment.make_alphanumeric(text)
    return QrSegment.make_bytes(payload)


def reference(payload, version, mask):
    """qrcodegen's matrix, packed as main/qrcode.c packs its module buffer:
    row-major, most significant bit first, trailing bits zero."""
    code = QrCode.encode_segments([segment(payload)], QrCode.Ecc.LOW, version, version, mask, False)
    size = code.get_size()
    bits = [code.get_module(x, y) for y in range(size) for x in range(size)]
    packed = bytes(
        sum(1 << (7 - j) for j in range(8) if i + j < len(bits) and bits[i + j]) for i in range(0, len(bits), 8)
    )
    image = numpy.full(((size + 8) * 4, (size + 8) * 4), 255, numpy.uint8)
    for y in range(size):
        for x in range(size):
            if code.get_module(x, y):
                image[(y + 4) * 4 : (y + 5) * 4, (x + 4) * 4 : (x + 5) * 4] = 0
    decoded = zxingcpp.read_barcodes(image, formats=zxingcpp.BarcodeFormat.QRCode)
    return packed, decoded[0].bytes if decoded else None


def main():
    rows = []
    for (name, payload, text), (version, mask, ours) in zip(PAYLOADS, encode_ours(PAYLOADS)):
        version, mask = int(version), int(mask)
        if not version:
            raise SystemExit(f"{name}: main/qrcode.c will not encode it")
        packed, decoded = reference(payload, version, mask)
        if decoded != payload:
            raise SystemExit(f"{name}: zxing-cpp read {decoded!r} from qrcodegen's code, not the payload")
        if packed.hex() != ours:
            raise SystemExit(f"{name}: main/qrcode.c's matrix differs from qrcodegen's at version {version}, mask {mask}")
        rows.append((name, payload, text, version, mask, packed))

    def array(data):
        return ", ".join(f"0x{b:02x}" for b in data)

    lines = [
        "/* Generated by tools/make_qr_vectors.py. Do not edit. Each matrix is Nayuki's",
        " * qrcodegen 1.8.0 encoding of the payload at error correction level L and the",
        " * version and mask given, read back to the payload by zxing-cpp 3.1.1, packed",
        " * as main/qrcode.c packs its module buffer. */",
        "",
        "typedef struct {",
        "    const char* name;",
        "    /* Through qrcode_initText rather than qrcode_initBytes. */",
        "    bool text;",
        "    const uint8_t* payload;",
        "    uint16_t length;",
        "    uint8_t version;",
        "    uint8_t mask;",
        "    const uint8_t* modules;",
        "} qr_vector_t;",
        "",
    ]
    for name, payload, text, version, mask, packed in rows:
        lines.append(f"static const uint8_t {name}_payload[] = {{ {array(payload)} }};")
        lines.append(f"static const uint8_t {name}_modules[] = {{ {array(packed)} }};")
    lines += ["", "static const qr_vector_t qr_vectors[] = {"]
    for name, payload, text, version, mask, packed in rows:
        lines.append(
            f'    {{ "{name}", {"true" if text else "false"}, {name}_payload, {len(payload)}, {version}, {mask}, {name}_modules }},'
        )
    lines += ["};", ""]
    OUTPUT.write_text("\n".join(lines))
    print(f"{OUTPUT.relative_to(ROOT)}: {len(rows)} vectors, all matching qrcodegen and decoded by zxing-cpp")


if __name__ == "__main__":
    main()
