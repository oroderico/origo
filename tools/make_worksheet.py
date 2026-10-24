#!/usr/bin/env python3
"""Write docs/worksheet.pdf, the printed tables every entropy method is checked
against.

Origo makes each word straight from what was rolled, flipped or drawn, and
shows it before using it, so the reader can check every word by hand - given
a table. This prints one: a guide to how each method is done and converted to
a word number by hand, a grid to record a run, and all 2048 words with the coin flips, D6 rolls, D20 rolls and
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


# Advance widths of printable ASCII, space to tilde, in thousandths of the font
# size, from the base fonts' metrics; Courier is 600 throughout. The PDF
# carries no widths, so the guide wraps its own lines with these.
HELVETICA = (
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556,
    556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667,
    556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556,
    556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722,
    500, 500, 500, 334, 260, 334, 584,
)
HELVETICA_BOLD = (
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556,
    556, 556, 556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722,
    611, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556,
    611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778,
    556, 556, 500, 389, 280, 389, 584,
)
EXTRA_WIDTHS = {"×": 584}


def text_width(font, points, text):
    if font.startswith("C"):
        return 0.6 * points * len(text)
    table = HELVETICA_BOLD if font == "HB" else HELVETICA
    return points * sum(table[ord(c) - 32] if " " <= c <= "~" else EXTRA_WIDTHS[c] for c in text) / 1000


def wrap(font, points, text, width):
    lines, line = [], ""
    for word in text.split():
        trial = f"{line} {word}" if line else word
        if line and text_width(font, points, trial) > width:
            lines.append(line)
            line = word
        else:
            line = trial
    return lines + [line]


# The opening pages: what the sheet is, how a run goes, and each method's
# conversion by hand, with the worked examples of docs/entropy.md. Each inner
# list is a section, kept on one page. Blocks: a title, a heading, a paragraph
# (bold with "b"), a bulleted list, numbered steps, and Courier lines.
GUIDE = [
    [
        ("title", "Origo worksheet"),
        ("p", "How coin flips, dice and SeedPicker's tickets become a BIP39 seed, one word at a time, and how to "
              "check every word by hand. After these pages come a grid to write a run on and the lookup: all 2048 "
              "words, each with the flips, rolls and ticket that make it."),
        ("b", "The printed sheet holds no secret: it is the same for everyone. It becomes your seed the moment you "
              "write a run on it. From then on keep it like the seed, or destroy it once the words are copied, and "
              "never photograph or scan it."),
    ],
    [
        ("h", "What you need"),
        ("p", "A pen, the Origo device, and one of:"),
        ("list", (
            "Coin flips: any coin.",
            "D6 dice: one or more ordinary six-sided dice.",
            "D20 dice: one or more twenty-sided dice.",
            "SeedPicker tickets: the 342 tickets T1 to T342 from seedpicker.net, printed, cut out and put in a bag "
            "or bowl, and one six-sided die. Origo is not affiliated with SeedPicker.",
        )),
        ("mono", (
            "               12 words              24 words",
            "Coin flips     128 flips             256 flips",
            "D6 dice        66 rolls + 7 flips    138 rolls + 3 flips",
            "D20 dice       33 rolls + 7 flips    69 rolls + 3 flips",
            "Tickets        11 draws + 7 flips    23 draws + 3 flips",
        )),
        ("p", "Rolls are counted as the rolls that count: the dice take more throws than that, as their sections "
              "explain."),
    ],
    [
        ("h", "The idea: eleven bits name a word"),
        ("p", "The BIP39 list has 2048 words, numbered here and on Origo from 0001 (abandon) to 2048 (zoo). 2048 is "
              "2 to the 11th power, so eleven binary digits - bits, each 0 or 1 - name exactly one word. Every "
              "method makes those eleven bits straight from what you flipped, rolled or drew, in order: nothing is "
              "hashed and nothing is estimated. A word's number is its eleven bits read as a binary number, plus 1."),
    ],
    [
        ("h", "Converting bits to a word number"),
        ("p", "Write the eleven bits under these place values, add up the place values with a 1 under them, then "
              "add 1:"),
        ("mono", (
            "place  1024  512  256  128   64   32   16    8    4    2    1",
            "bits      1    1    0    1    0    0    1    0    1    0    0",
            "",
            "1024 + 512 + 128 + 16 + 4 = 1684        1684 + 1 = 1685  ->  spoon",
        )),
        ("p", "Back from a number to bits: subtract 1, then go down the place values from 1024, writing 1 and "
              "subtracting the place value when it fits, 0 when it does not. 1685 - 1 = 1684: 1024 fits (660 "
              "left), 512 fits (148), 256 not, 128 fits (20), 64 not, 32 not, 16 fits (4), 8 not, 4 fits (0), 2 "
              "not, 1 not: 11010010100."),
        ("p", "The lookup has done this for every word, so a run needs no arithmetic; it is here so that the "
              "tables themselves can be checked. Each method below also has its own formula in ordinary numbers."),
    ],
    [
        ("h", "A run, step by step"),
        ("steps", (
            "On Origo choose New Seed, From entropy, the method, then 12 words or 24 words. Origo opens on the "
            "method's rule.",
            "Write the method and the length at the top of the recording grid.",
            "For each word, make its entries - flips, rolls, or a ticket and a die - and write them in that word's "
            "row as you enter them on Origo, one at a time. Each entry can be undone.",
            "Origo then shows what made the word, its number and the word - 4, 2, 1, 3, 3, 2 and 1685 spoon, say. "
            "Find that number in the lookup and check that its row, in your method's column, matches what "
            "you wrote. If it does, write the number and the word in the grid and tick it. If not, decline: that "
            "word's entries are thrown away and asked for again.",
            "After the last word Origo lists the whole run, four words a page. Compare it with the grid in one "
            "pass.",
            "Flip the final 7 or 3 coins and write them in the grid's last row. Origo completes the last word "
            "(see Finishing) and shows the mnemonic.",
            "Copy the mnemonic to where it will be kept, and check the copy against Origo.",
        )),
    ],
    [
        ("h", "Coin flips"),
        ("p", "Eleven flips make a word. Heads is 1, tails is 0, written in the order flipped: the first flip is "
              "the leftmost bit, place value 1024."),
        ("mono", (
            "flips  H H T H   T T H T   H T T",
            "bits   1 1 0 1   0 0 1 0   1 0 0     ->  11010010100  ->  1685  spoon",
        )),
        ("p", "In the lookup the Coins column is the eleven flips as 1s and 0s, in groups of 4, 4 and 3 to keep "
              "your place: 1101 0010 100."),
    ],
    [
        ("h", "D6 dice"),
        ("p", "Only faces 1 to 4 count. Roll a 5 or a 6 again before entering anything: counting them would make "
              "some words likelier than others, and Origo does not offer them. Each counted face gives two bits, "
              "the face minus 1 in binary:"),
        ("mono", (
            "face   1    2    3    4",
            "bits   00   01   10   11",
        )),
        ("p", "A word is six rolls. The first five give two bits each; the sixth gives only one bit, the first of "
              "its two, so on the last roll 1 or 2 means 0 and 3 or 4 means 1."),
        ("mono", (
            "rolls  4    2    1    3    3    2",
            "bits   11   01   00   10   10   0    ->  11010010100  ->  1685  spoon",
        )),
        ("p", "Without bits: number = (1st - 1) × 512 + (2nd - 1) × 128 + (3rd - 1) × 32 + (4th - 1) "
              "× 8 + (5th - 1) × 2 + (0 if the 6th is 1 or 2, 1 if it is 3 or 4) + 1. Here 1536 + 128 + "
              "0 + 16 + 4 + 0 + 1 = 1685."),
        ("p", "In the lookup the D6 column is the first five rolls, then the last roll's pair: 4 2 1 3 3 1-2. "
              "Either face of the pair makes the word."),
    ],
    [
        ("h", "D20 dice"),
        ("p", "Only faces 1 to 16 count; roll 17, 18, 19 or 20 again. Each counted face gives four bits, the face "
              "minus 1 in binary:"),
        ("mono", (
            "face   1    2    3    4    5    6    7    8",
            "bits   0000 0001 0010 0011 0100 0101 0110 0111",
            "face   9    10   11   12   13   14   15   16",
            "bits   1000 1001 1010 1011 1100 1101 1110 1111",
        )),
        ("p", "A word is three rolls. The first two give four bits each; the third gives only its first three, so "
              "on the last roll the faces pair up: 1 or 2, 3 or 4, and so on to 15 or 16."),
        ("mono", (
            "rolls  16     1      10",
            "bits   1111   0000   100             ->  11110000100  ->  1925  vacuum",
        )),
        ("p", "The last roll, 10, is 1001; its first three bits, 100, are kept. Without bits: number = (1st - 1) "
              "× 128 + (2nd - 1) × 8 + half of (3rd - 1), rounded down, + 1. Here 1920 + 0 + 4 + 1 = 1925."),
        ("p", "In the lookup the D20 column is the first two rolls, then the last roll's pair: 16 1 9-10."),
    ],
    [
        ("h", "SeedPicker tickets"),
        ("p", "For each word, draw a ticket from the bag, note it, put it back and mix; then roll the six-sided "
              "die, on which every face counts. The pair names word number (ticket - 1) × 6 + die."),
        ("mono", (
            "T57, die 4   ->  (57 - 1) x 6 + 4 = 340   ->  0340  clerk",
        )),
        ("p", "342 tickets and 6 faces make 2052 pairs: the 2048 words once each, and four more, T342 with a 3, 4, "
              "5 or 6, which are past 2048. On those, draw a new ticket - not just a new die, which would make "
              "T342's two words three times as likely as any other. Origo says so when it happens."),
        ("p", "In the lookup the Ticket column is each word's pair: T57+4. SeedPicker's own printed table gives the "
              "same pairs."),
    ],
    [
        ("h", "Finding a row fast"),
        ("p", "The lookup rows are in number order, which is also the order of every column read left to right, "
              "as in a dictionary. So the first entry already narrows the search, and each later one narrows it "
              "again:"),
        ("mono", (
            "Coins   first flip  T: 0001-1024   H: 1025-2048",
            "D6      first roll  1: 0001-0512   2: 0513-1024   3: 1025-1536   4: 1537-2048",
            "D20     first roll  f: (f - 1) x 128 + 1 to f x 128, so 16: 1921-2048",
            "Ticket  ticket T:   (T - 1) x 6 + 1 to (T - 1) x 6 + 6; the die picks one",
        )),
        ("p", "Within a D6 first roll each second roll is 128 words and each third 32; within a D20 first roll each "
              "second roll is 8 words. Each lookup page is headed with the numbers it holds."),
    ],
    [
        ("h", "Finishing: the last word"),
        ("p", "A 12-word seed is 128 bits of yours and a 4-bit checksum, 132 = 12 × 11. Eleven words make 121 "
              "bits, so 7 coin flips finish your 128. For 24 words, 23 words make 253 bits, 3 flips finish 256, "
              "and the checksum is 8 bits. Origo then computes the checksum, from SHA-256 of your bits, which no "
              "table can do, and with it the last word."),
        ("p", "Part of the last word can still be checked here: its first bits are your final flips. For 12 words "
              "the last word's Coins column starts with your 7 flips, for 24 with your 3. Seven flips H T T H H T "
              "H are 1001101, so the twelfth word is one of the 16 rows whose Coins begin 1001 101, numbers 1233 "
              "to 1248. The checksum itself can be checked on an offline computer with tools/origo_verify.py."),
        ("p", "SeedPicker's calculator always takes the alphabetically first valid 24th word: three tails give that "
              "same word."),
    ],
    [
        ("h", "Words already chosen"),
        ("p", "Words drawn elsewhere - SeedPicker's own form, a metal plate, any other table - are finished with "
              "New Seed, Complete checksum, then 11 words + 7 coins or 23 words + 3 coins. Type each word, or "
              "choose Enter word numbers and type its number from the lookup; then flip the coins as above."),
    ],
    [
        ("h", "Good practice"),
        ("list", (
            "Work alone, with no phone or camera that can see the sheet, the dice or the screen.",
            "Roll dice from a cup onto a flat, hard surface, and flip coins high enough to turn several times. "
            "Never choose a face or a ticket: every entry must come from the throw or the draw.",
            "Write each entry down before entering it, so the sheet and the device can be compared.",
            "Use one blank grid per seed, and never reuse a run.",
            "When the mnemonic is safely copied, keep the filled sheet with it or destroy it - burnt, or "
            "cross-cut shredded - and never leave it in a bin whole.",
        )),
    ],
]

LEADING = 1.32


def guide_lines(section, measure):
    """A section as lines to set: (font, points, indent, text, advance)."""
    for kind, body in section:
        if kind == "title":
            yield "HB", 18, 0, body, 26
        elif kind == "h":
            yield "HB", 11.5, 0, body, 16
        elif kind in ("p", "b"):
            font = "HB" if kind == "b" else "H"
            for line in wrap(font, 9.5, body, measure):
                yield font, 9.5, 0, line, 9.5 * LEADING
            yield None, 0, 0, "", 4
        elif kind in ("list", "steps"):
            for number, item in enumerate(body, start=1):
                mark = f"{number}." if kind == "steps" else "-"
                for line_number, line in enumerate(wrap("H", 9.5, item, measure - 16)):
                    if not line_number:
                        yield "H", 9.5, 2, mark, 0
                    yield "H", 9.5, 16, line, 9.5 * LEADING
                yield None, 0, 0, "", 2
            yield None, 0, 0, "", 2
        elif kind == "mono":
            for line in body:
                yield "C", 9, 8, line, 9 * 1.3
            yield None, 0, 0, "", 5
        else:
            raise ValueError(kind)


def guide_pages(size):
    """The guide, flowed over as many pages as it takes, each section kept
    whole on one page."""
    width, height = size
    measure = width - 2 * MARGIN
    bottom = height - MARGIN
    pages, top = [Page(height)], MARGIN
    for section in GUIDE:
        lines = list(guide_lines(section, measure))
        length = sum(line[4] for line in lines)
        if length > bottom - MARGIN:
            raise RuntimeError("a guide section is longer than a page")
        gap = 8 if top > MARGIN else 0
        if top + gap + length > bottom:
            pages.append(Page(height))
            top, gap = MARGIN, 0
        top += gap
        for font, points, indent, text, advance in lines:
            if font:
                pages[-1].text(MARGIN + indent, top, font, points, text)
            top += advance
    return pages

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

    pages = guide_pages(size)

    grid = Page(height)
    grid.text(MARGIN, MARGIN, "HB", 14, "Record a run")
    grid.text(MARGIN, MARGIN + 20, "H", 9, "One row per word. Tick the last column once the lookup row matches what you wrote.")
    grid.text(MARGIN, MARGIN + 38, "H", 10, "Method")
    grid.line(MARGIN + 40, MARGIN + 48, MARGIN + 220, MARGIN + 48, 0.4, 0.5)
    grid.text(MARGIN + 250, MARGIN + 38, "H", 10, "Words:   12   /   24")
    columns = (("Word", 34), ("Rolls, flips or draw", 220), ("No.", 50), ("Word", 120), ("Checked", 0))
    grid_top, row_height = MARGIN + 64, (height - 2 * MARGIN - 90) / 25
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

    for number, page in enumerate(pages, start=1):
        page.text(width - MARGIN - 70, MARGIN + 2, "H", 8, f"Page {number} of {len(pages)}", 0.35)
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
