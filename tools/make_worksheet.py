#!/usr/bin/env python3
"""Write docs/worksheet.pdf, the printed tables every entropy method is checked
against.

Origo makes each word straight from what was rolled, flipped or drawn, and
shows it before using it, so the reader can check every word by hand - given
a table. This prints one: the rules and the faces-to-bits tables, a grid to
record a run, and all 2048 words with the coin flips, D6 rolls, D20 rolls and
SeedPicker ticket that make each, so any method is read off by eye with no
arithmetic.

The PDF is written by hand with the base-14 fonts, no dates and no IDs, so the
same source always gives the same bytes and the committed file can be checked
against this script. Run by hand when the tables change. Standard library
only, like the rest of tools/.

    python3 tools/make_worksheet.py [--paper a4|letter] [--out path]
"""

import argparse
from pathlib import Path
import zlib

ROOT = Path(__file__).parents[1]
OUTPUT = ROOT / "docs/worksheet.pdf"
WORDLIST = ROOT / "components/libwally-core/upstream/src/data/wordlists/english.txt"
PAPERS = {"a4": (595, 842), "letter": (612, 792)}
MARGIN = 36

# The methods, as the firmware reads them (main/seedtool_core.c,
# seedtool_die_word and seedtool_ticket_word): a die's counted faces and the
# bits each gives, and SeedPicker's ticket count and die.
D6_BITS, D6_ROLLS = 2, 6
D20_BITS, D20_ROLLS = 4, 3
WORD_BITS = 11
TICKETS, TICKET_DIE = 342, 6


def words():
    result = WORDLIST.read_text(encoding="ascii").splitlines()
    if len(result) != 2048:
        raise RuntimeError("bundled BIP39 English wordlist is missing or invalid")
    return result


def die_faces(index, bits, rolls):
    """The faces that make word `index` on a die whose counted faces give
    `bits` each. Every roll but the last is one face; the last keeps only the
    bits the word still needs, so it is a run of faces, any of which will do:
    (first, last)."""
    last_bits = WORD_BITS - bits * (rolls - 1)
    faces = [((index >> (WORD_BITS - bits * (r + 1))) & ((1 << bits) - 1)) + 1 for r in range(rolls - 1)]
    value = index & ((1 << last_bits) - 1)
    spread = 1 << (bits - last_bits)
    return faces, (value * spread + 1, value * spread + spread)


def ticket(index):
    """SeedPicker's ticket and die for word `index`: word number
    (ticket - 1) x 6 + die."""
    return index // TICKET_DIE + 1, index % TICKET_DIE + 1


def rows():
    """One row per word: number, word, coin flips, D6 rolls, D20 rolls and
    SeedPicker draw, as printed."""
    result = []
    for index, word in enumerate(words()):
        bits = f"{index:011b}"
        d6, (d6_low, d6_high) = die_faces(index, D6_BITS, D6_ROLLS)
        d20, (d20_low, d20_high) = die_faces(index, D20_BITS, D20_ROLLS)
        t, die = ticket(index)
        result.append((
            f"{index + 1:04}",
            word,
            f"{bits[:4]} {bits[4:8]} {bits[8:]}",
            " ".join(str(f) for f in d6) + f" {d6_low}-{d6_high}",
            " ".join(f"{f:2}" for f in d20) + f" {d20_low}-{d20_high}",
            f"T{t}+{die}",
        ))
    return result


def escape(text):
    """A PDF string literal's body, in the base fonts' WinAnsi encoding."""
    raw = text.encode("cp1252")
    return raw.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)")


class Page:
    """One page's content stream, in PDF user space (origin bottom left)."""

    def __init__(self, height):
        self.height = height
        self.ops = []

    def text(self, x, top, font, size, text, grey=0.0):
        y = self.height - top - size
        self.ops.append(b"%.2f g BT /%s %.2f Tf %.2f %.2f Td (" % (grey, font.encode(), size, x, y)
                        + escape(text) + b") Tj ET")

    def rect(self, x, top, width, height, grey):
        self.ops.append(b"%.2f g %.2f %.2f %.2f %.2f re f" % (grey, x, self.height - top - height, width, height))

    def line(self, x1, top1, x2, top2, width=0.5, grey=0.0):
        self.ops.append(b"%.2f G %.2f w %.2f %.2f m %.2f %.2f l S"
                        % (grey, width, x1, self.height - top1, x2, self.height - top2))

    def stream(self):
        return b"\n".join(self.ops)


FONTS = {"H": "Helvetica", "HB": "Helvetica-Bold", "C": "Courier", "CB": "Courier-Bold"}


def write_pdf(pages, size):
    """Pages to PDF bytes: a catalog, a page tree, the four base fonts, and a
    page and a compressed content stream per page. No info dictionary and no
    ID, so nothing varies between runs."""
    width, height = size
    objects = [None, None]
    font_ids = {}
    for name, base in FONTS.items():
        objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding >>" % base.encode())
        font_ids[name] = len(objects)
    fonts = b" ".join(b"/%s %d 0 R" % (name.encode(), font_ids[name]) for name in FONTS)
    kids = []
    for page in pages:
        data = zlib.compress(page.stream(), 9)
        objects.append(b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(data) + data + b"\nendstream")
        content = len(objects)
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %d %d] /Resources << /Font << %s >> >> "
                       b"/Contents %d 0 R >>" % (width, height, fonts, content))
        kids.append(len(objects))
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objects[1] = b"<< /Type /Pages /Kids [%s] /Count %d >>" % (b" ".join(b"%d 0 R" % k for k in kids), len(kids))

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    out += b"".join(b"%010d 00000 n \n" % offset for offset in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref)
    return bytes(out)


# Page 1: the rule, each method's table, and worked examples taken from
# docs/entropy.md. Lines are broken by hand: the base fonts carry no widths
# here, so every line is kept well inside the margins.
GUIDE = [
    ("HB", 18, "Origo worksheet"),
    None,
    ("H", 10, "Every word is eleven bits, made straight from what you roll, flip or draw - no hash in between."),
    ("H", 10, "Find each word in the lookup pages under your method's column; Origo shows the same number"),
    ("H", 10, "and word before it uses it, so every word can be checked by hand as it is made."),
    ("HB", 10, "Once a run is written on this sheet, the sheet is your seed: keep it or destroy it like one."),
    None,
    ("HB", 12, "Coin flips"),
    ("H", 10, "Eleven flips per word, in order: heads is 1, tails is 0. The eleven digits are the Coins column."),
    None,
    ("HB", 12, "D6 dice - only 1 to 4 count; roll 5 or 6 again"),
    ("C", 10, "face   1    2    3    4"),
    ("C", 10, "bits   00   01   10   11"),
    ("H", 10, "Six rolls per word. The last keeps only one bit, so the D6 column ends in 1-2 or 3-4: either"),
    ("H", 10, "face of that pair makes the word."),
    None,
    ("HB", 12, "D20 dice - only 1 to 16 count; roll 17 to 20 again"),
    ("C", 10, "face   1    2    3    4    5    6    7    8"),
    ("C", 10, "bits   0000 0001 0010 0011 0100 0101 0110 0111"),
    ("C", 10, "face   9    10   11   12   13   14   15   16"),
    ("C", 10, "bits   1000 1001 1010 1011 1100 1101 1110 1111"),
    ("H", 10, "Three rolls per word. The last keeps three bits, so the D20 column ends in a pair such as 9-10."),
    None,
    ("HB", 12, "SeedPicker tickets (seedpicker.net; Origo is not affiliated)"),
    ("H", 10, "Draw a ticket T1 to T342, put it back, roll a six-sided die: word number = (ticket - 1) x 6 + die."),
    ("H", 10, "T342 with a 3, 4, 5 or 6 is past 2048: draw a new ticket. The Ticket column is each word's pair."),
    None,
    ("HB", 12, "Finishing"),
    ("H", 10, "12 words: make 11 words, then flip 7 more coins on the device. 24 words: 23 words and 3 coins."),
    ("H", 10, "BIP39's checksum then completes the last word, the one thing no table can give."),
    None,
    ("HB", 12, "Worked examples"),
    ("C", 10, "Coins   1101 0010 100        ->  1685  spoon"),
    ("C", 10, "D6      4, 2, 1, 3, 3, 2     ->  1685  spoon   (last roll 1-2)"),
    ("C", 10, "D20     16, 1, 10            ->  1925  vacuum  (last roll 9-10)"),
    ("C", 10, "Ticket  T57 + die 4          ->  0340  clerk"),
]

LOOKUP_HEADING = ("No.", "Word", "Coins", "D6", "D20", "Ticket")
# Characters per column of a lookup row, and the gap between columns. Each
# cell is placed at its own x rather than padded with spaces, so the columns
# line up even where a viewer substitutes a bold Courier of another width.
LOOKUP_WIDTHS = (4, 8, 13, 13, 11, 6)
LOOKUP_GAP = 1.3
LOOKUP_POINTS = 6.8


def lookup_cells(page, x, top, font, cells):
    advance = 0.6 * LOOKUP_POINTS
    for cell, width in zip(cells, LOOKUP_WIDTHS):
        page.text(x, top, font, LOOKUP_POINTS, cell)
        x += (width + LOOKUP_GAP) * advance


def build(paper):
    size = PAPERS[paper]
    width, height = size
    pages = []

    guide = Page(height)
    top = MARGIN
    for entry in GUIDE:
        if entry is None:
            top += 8
            continue
        font, points, text = entry
        guide.text(MARGIN, top, font, points, text)
        top += points * 1.35
    pages.append(guide)

    grid = Page(height)
    grid.text(MARGIN, MARGIN, "HB", 14, "Record a run")
    grid.text(MARGIN, MARGIN + 20, "H", 9, "One row per word. Tick the last column once the device shows the same number and word.")
    columns = (("Word", 34), ("Rolls, flips or draw", 220), ("No.", 50), ("Word", 120), ("Checked", 0))
    grid_top, row_height = MARGIN + 44, (height - 2 * MARGIN - 70) / 25
    x = MARGIN
    for name, column_width in columns:
        grid.text(x + 3, grid_top, "HB", 9, name)
        x += column_width
    for row in range(25):
        y = grid_top + 14 + row * row_height
        grid.line(MARGIN, y, width - MARGIN, y, 0.4, 0.5)
        label = f"{row + 1}" if row < 24 else "Final coins (7 or 3)"
        grid.text(MARGIN + 3, y + (row_height - 9) / 2, "H", 9, label, 0.35)
    y_bottom = grid_top + 14 + 25 * row_height
    grid.line(MARGIN, y_bottom, width - MARGIN, y_bottom, 0.4, 0.5)
    x = MARGIN
    for _, column_width in columns[:-1]:
        x += column_width
        grid.line(x, grid_top + 14, x, y_bottom - row_height, 0.4, 0.5)
    pages.append(grid)

    all_rows = rows()
    column_gap = 22
    column_width = (width - 2 * MARGIN - column_gap) / 2
    first_row = MARGIN + 30
    # As few pages as the tightest pitch allows, then the rows spread evenly
    # over them, so the last page is not a few stragglers.
    room = height - MARGIN - first_row - 10
    most = 2 * int(room // (LOOKUP_POINTS * 1.2))
    page_count = (len(all_rows) + most - 1) // most
    per_column = -(-len(all_rows) // (2 * page_count))
    per_page = 2 * per_column
    pitch = min(LOOKUP_POINTS * 1.3, room / per_column)
    for page_index in range(page_count):
        page = Page(height)
        start = page_index * per_page
        page.text(MARGIN, MARGIN, "HB", 11, f"Lookup: words {start + 1:04} to {min(start + per_page, 2048):04}")
        page.text(width - MARGIN - 70, MARGIN + 2, "H", 8, f"Page {page_index + 3} of {page_count + 2}", 0.35)
        for side in range(2):
            x = MARGIN + side * (column_width + column_gap)
            if start + side * per_column >= len(all_rows):
                break
            lookup_cells(page, x, first_row - pitch - 2, "CB", LOOKUP_HEADING)
            for line in range(per_column):
                position = start + side * per_column + line
                if position >= len(all_rows):
                    break
                y = first_row + line * pitch
                # A light band every other five rows keeps the eye on a row.
                if (position // 5) % 2:
                    page.rect(x - 2, y - 1.2, column_width + 4, pitch, 0.92)
                lookup_cells(page, x, y, "C", all_rows[position])
        pages.append(page)

    guide.text(width - MARGIN - 70, MARGIN + 4, "H", 8, f"Page 1 of {len(pages)}", 0.35)
    grid.text(width - MARGIN - 70, MARGIN + 2, "H", 8, f"Page 2 of {len(pages)}", 0.35)
    return write_pdf(pages, size)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--paper", choices=sorted(PAPERS), default="a4")
    parser.add_argument("--out", type=Path, default=OUTPUT)
    args = parser.parse_args()
    args.out.write_bytes(build(args.paper))
    print(f"{args.out}: {args.paper}")


if __name__ == "__main__":
    main()
