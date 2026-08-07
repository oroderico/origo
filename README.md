# Origo

*Not your entropy, not your coins.*

Origo is a dedicated, stateless firmware for the original LILYGO
TTGO T-Display (ESP32, 240x135, two buttons). It is not a hardware wallet. It
does not sign, store a seed, expose an RPC protocol, update itself, or start a
radio. USB/UART is used only by the ESP32 ROM bootloader for power and flashing.

Origo is an independent project derived from parts of Blockstream Jade. It is
not affiliated with or endorsed by Blockstream.

The device never supplies entropy to a mnemonic. ESP32 random bytes are used
only once to blind libsecp256k1's context against side-channel analysis. The
seed-generation functions have no RNG input and produce the same result if the
device RNG is stubbed.

## Why

On 30 July 2026, attackers began draining Coldcard wallets: an initial wave
took 1,196 addresses (about $70.2M) in 41 minutes, with further waves
bringing the publicly reported total to roughly $88.6M across some 4,585
addresses. The root cause, per Coinkite's disclosure and public reporting,
was a firmware regression dating to March 2021: a misconfigured build flag
had silently routed seed generation on Mk3, Mk4, Q and early Mk5 units to
MicroPython's Yasmarang fallback PRNG — seeded only from the chip's unique ID
and timer state — instead of the STM32's hardware RNG, producing roughly 72
bits of real entropy where 128 were assumed. It went unnoticed for over five
years. Coinkite's fix, shipped the next day, cannot repair a seed already
generated on the flawed firmware: every affected owner has to generate a new
one and move their funds.

Origo exists to make that class of bug structurally impossible rather than
merely unlikely. There is no RNG-to-seed code path for a misconfigured build
flag to misroute in the first place: a mnemonic is a pure function of a
canonical transcript the human recorded by hand — dice, coins or cards —
reduced with SHA256, and, as stated just above, the device RNG plays no part
in it. A firmware regression that silently swapped the entropy source behind
a device owner's back, the same shape of bug that cost Coldcard users tens of
millions of dollars, has nothing to act on here: stub the RNG out entirely
and Origo generates the identical mnemonic.

## Entropy transcripts

The transcript is shown before its full SHA256 and mnemonic. Record it so the
calculation can be reproduced independently.

While the entries are being keyed in, the transcript so far is shown under the
value being entered, so a mis-keyed roll is caught against the paper then rather
than after ninety-nine of them. Only the tail that fits one line is drawn, which
is the part that just changed, and stepping back unwrites the last entry. What is
on screen is always a prefix of the finished transcript — the self-test checks
that for every prefix length of every source, since a separator that appeared
only later would mean checking a string the device then rewrites.

| Source | 12 words | 24 words | Canonical transcript |
|---|---:|---:|---|
| D6 | 50 rolls | 99 rolls | digits concatenated, e.g. `123456` |
| D20 | 30 rolls | 60 rolls | decimal rolls joined by `-`, e.g. `1-20-7` |
| Coins | 128 flips | 256 flips | Heads=`1`, Tails=`0`, concatenated |
| Cards | first 25 distinct cards | first 48 cards, with replacement | `cards-v1:` plus rank/suit codes |

Card ranks are `A23456789TJQK`; suits are `CDHS`. The canonical deck order is
`AC..KC`, `AD..KD`, `AH..KH`, `AS..KS`. 12-word draws the first 25 distinct
cards from one deck; without replacement, a single 52-card deck tops out
around `log2(52!) ≈ 225.6` bits, short of a 24-word mnemonic's 256 even
drawing every card, and the 52nd card would add nothing anyway once the
other 51 are known. 24 words instead returns each card and reshuffles before
drawing the next, so a repeat is expected and valid there - the encoding is
otherwise identical, just longer and, being drawn with replacement, graded
the same estimated way as dice and coins (see below) rather than the exact
count 12-word cards uses.

Turning that transcript into words is three mechanical steps, always in this
order, and nothing else touches them: no RNG, no timestamp, no per-device
salt.

1. SHA256 of the ASCII transcript bytes — the full 32-byte digest, every
   time, regardless of source or word count.
2. Truncate to the first 16 bytes for a 12-word mnemonic, or keep all 32 for a
   24-word one.
3. Hand those bytes to libwally as BIP39 entropy exactly as the standard
   defines it: a checksum is computed from SHA256 of the entropy itself and
   appended as its last few bits, and the combined bit string is split into
   11-bit groups, each one a wordlist index.

The same transcript always produces the same mnemonic. This is
`bip39_mnemonic_from_bytes(NULL, hash, entropy_len, &mnemonic)` in
`main/seedtool_core.c`, called once, on that hash and nothing else —
`tools/origo_verify.py generate` reimplements the same three steps
independently, in Python, so the whole pipeline can be checked without
trusting the device that ran it.

Checksum completion (11 or 23 already-known words, plus 7 or 3 coin flips)
gets its entropy differently: the known words' own 11-bit wordlist positions
are packed back-to-back to *become* the entropy bytes directly — not hashed,
not derived from anything — and the coin flips fill in the low-order bits a
full BIP39 checksum would otherwise occupy. That buffer is handed to the same
`bip39_mnemonic_from_bytes` call, which computes the real checksum from it and
picks whichever one final word makes it valid. The coin flips are the only
literal randomness this path needs, and they must be exactly as many bits as
are missing — one too few or too many and the call is rejected outright rather
than guessing.

Neither path ever reads the device RNG. The entropy quality bar described
next grades what has already been typed; it cannot add or remove a single bit
from what gets hashed.

Every source's run opens on a screen naming how many rolls, flips or cards it
needs and what the bar below is about, with that bar already in place but
empty. While entries are being keyed in, its two segments track draws
collected and bits so far, each against the mnemonic's minimum, and the
entropy segment turns red if the run looks patterned — an arithmetic run such
as `1,2,3,4,5,6,1,2,3,...` for dice and coins, or a fresh unshuffled deck read
straight through for cards, rather than a real draw. Once all the required
entries are in, poor entropy or a detected pattern is confirmed before the
mnemonic is generated; declining any of these screens steps back to redo the
last entry. With neither problem, the bar's outline turns green on one last
"Entropy looks good" screen before generating. All of this is a pure function
of what's already been entered: a UI quality signal only, never an input to
the transcript that gets hashed.

D6, D20 and coin flips share the same plug-in Shannon-entropy estimator — a
coin is read as a two-sided die for this purpose only, not the 0/1 its own
transcript encoding uses, and (unlike dice) is collected at exactly the
theoretical minimum with no cushion, so an honest run has a small (empirically
under 10%, checked by a self-test) chance of tripping the "poor entropy"
screen once on the way to "proceed anyway". 12-word cards are drawn without
replacement, so their bits are exact rather than estimated —
`log2(52!/(52-drawn)!)`, the same number regardless of which cards came up —
and comfortably clear the 128-bit minimum by the 25th draw on their own; only
their pattern check (each draw's rank, ignoring suit) can ever flag a card
run. 24-word cards, drawn with replacement, go back to being an estimate —
graded as a genuine 52-sided die, the same plug-in estimator and the same
small (empirically under 1%, at the 48 draws that mode asks for) chance of
an honest run tripping "poor entropy" once. Adapted from Krux's dice-roll
entropy screen (github.com/selfcustody/krux).

Checksum completion consumes 11 BIP39 words plus exactly 7 coin flips, or 23
words plus exactly 3 flips. Those flips are the missing entropy bits and lead
to one checksum-valid final word; the firmware never randomly selects from the
128 or 8 otherwise-valid endings.

The opening screen shows the logo as drawn — mark, wordmark and tagline in one
picture — for a couple of seconds and then gives way to the menu on its own.
Presses are discarded while it is up: it is where the user is still learning that
both buttons together mean select, and a screen that offers no choice must not
turn a stray press into one.

That artwork is the only picture in the firmware: a 98x110 array of sixteen
palette entries at two pixels per byte, about 5.4 KiB, written straight into the
framebuffer by indexing that palette. It is deliberately not compressed. Deflate
would take it to roughly 2 KiB, but the decompressor is one of the components the
audit rejects by name, and trading an audited absence for two kilobytes in an
image that already has twelve to spare is a bad exchange. Sixteen colours are the
compression: they differ from full RGB565 by well under one percent on this
artwork, which is drawn with a limited palette to begin with.

`tools/make_logo.py` regenerates `main/seedtool_logo.c` from `assets/logo.png`
and is the one place Pillow is used; the firmware, the verifier and the tests
stay dependency-free.

## Controls

The board has two buttons and no select button. In a list, the left button
moves to the previous item and the right button to the next; both pressed
together select. Holding one button repeats it. A press is acted on only once
both buttons are released, so a chord is never mistaken for a step. A numeric
carousel (dice rolls, cards) reads the same two buttons the other way: left
raises the value, right lowers it, since there the physical button read as
"up" raising what is on screen matters more than reusing a list's sense of
"previous".

Pressing a button teaches left, right and hold. It cannot teach the chord, since
nothing moves until both buttons are released, so `L/R move   BOTH select` sits
under the first screens reachable from the main menu and disappears for good
once the chord has been used once — not under the main menu itself, which is
not the place to also be teaching it. What stays under a screen after that is
its position counter alone. The two footers that name a consequence rather
than a gesture — `BOTH continue   L/R back`, and the timeout's `BOTH extend
L/R erase` — are always shown, because guessing wrong there costs a session.

Coin flips are a direct choice rather than a carousel position: left picks
Heads, right picks Tails, one press per flip. With a run as long as 128 or 256
flips, halving the presses per flip halves the whole entry. Both buttons
together undoes the last flip, since there is no longer a neutral carousel
position to step onto for that.

A card is picked in two carousels rather than one: suit first, then rank
within it. Scanning the whole deck from `AC` for every one of the 25 cards a
seed needs averages around 26 presses per card; splitting a 4-way suit
carousel from a 13-way rank carousel cuts that to around 8.5. Stepping back
off the rank carousel returns to the suit carousel for the same card, one
stage back as everywhere else; only stepping back off the suit carousel
undoes the card before it.

Three gestures is all two buttons afford, and all three are spoken for, so going
back cannot be a button: it is a place on the screen. Every screen has one, and
it steps back exactly one stage rather than abandoning the flow. Menus carry a
`Back` row; the word list carries `[delete]`; deleting past the start of a word
returns to the previous word, and past the first word to the menu before it. The
numeric carousel carries `[back]` one step below its lowest value, so a misread
roll 29 of 50 is corrected by stepping back to it rather than by waiting out the
session timeout and starting the transcript again. Coin flips are the one
exception: both a value and its confirmation used to cost two of the three
gestures, so freeing the one that used to confirm turns it into "undo" instead
of a screen position, since a direct choice has nothing left to confirm.

The main menu's `Settings` entry carries `About` (the safety disclaimer),
`Flip Orientation`, which toggles the panel 180 degrees in place, and
`Brightness`, which steps the backlight through five PWM levels, applied live
as it is adjusted. The display settings are session-only - like every other
piece of UI state, they reset to their defaults (unflipped, full brightness)
on the next boot, since Origo has nothing to save them to.

Every choice is a list showing three options at once with the selection
highlighted, so an option is always read alongside its neighbours. Three rather
than five, because three rows leave room for the 16px face instead of the 11px
one and this is read at arm's length off a screen an inch across; the cost is
that more lists scroll. A longer list scrolls by the least it can and carries a
scrollbar down its right edge: the thumb is as tall a fraction of the track as
the visible rows are of the list, and it sits flush with the top on the first row
and flush with the bottom on the last. Three rows on their own say nothing about
how much is below them; the thumb says how much and where. The end of a list is
never padded with blank rows. Only labels are listed. Values meant to be transcribed are paged
instead, split by what fits the display.

The last row of every list with somewhere to return to is the way out of it —
`Back`, `Done / erase`, `[delete]` — and a rule is drawn above it so it is not
read as one more choice. That row is always last, in every such list, so
leaving a screen is always in the same place. The one list with nowhere to
return to is the main menu itself: there is no `Back` row there, and the
session-timeout wipe (`BOTH extend   L/R erase`) is the only way out of it
short of the board's own physical reset.

Entering a mnemonic asks first whether the words will be typed as letters or as
word numbers, and every word of that mnemonic is then entered the chosen way.

Typed as letters, only letters that still lead to a BIP39 word are reachable, and
once ten or fewer words remain they are listed outright. Typed as numbers, only
digits that still lead to a word number are reachable, and the number is shown as
the word it means, to be confirmed, before the next word is asked for. In both
cases deleting past the start of a word steps back to the previous one.

A word number is one-based: the position in a printed BIP39 English wordlist,
where `abandon` is 1 and `zoo` is 2048. It is one more than the zero-based index
the encoding itself uses, so a list numbered from zero must be read with that in
mind. `tools/origo_verify.py inspect` prints the same one-based numbers for a
mnemonic, and the self-test requires all 2048 of them to be typeable both plainly
and padded to four digits.

The keyboards are QWERTY. The cursor opens on the middle key of the middle row —
`g` on the letter keyboards, `5` on the number one — because on a ring of thirty
keys walked with two buttons the corner is the furthest possible place to start
from. It then keeps its place, so repeating a character is one press rather than
a walk back, and returns to the centre when a keyboard is opened or a symbol page
is turned. When the letter it was resting on stops leading to a word, it moves to
the nearest key that still does.

The suggestion order, the initially selected key and where a list has scrolled to
are pure functions of what has been typed and chosen. None is randomised: no
screen in the entry path may depend on the device RNG.

An optional printable-ASCII passphrase of at most 100 characters is typed on a
four-page keyboard covering the whole printable range, entered twice, and exists
for that derivation session only.

## Restore and inspect

Existing mnemonics are validated before derivation; a bad checksum blocks the
address viewer entirely. For a valid mnemonic the viewer shows the master
fingerprint, then one wallet type at a time — Native SegWit (BIP84) or Taproot
(BIP86) — chosen up front, so the account key and its addresses are always read
for the type just picked rather than interleaved with the other one's. Each
type shows:

- its watch-only account key at `m/84'/0'/0'` or `m/86'/0'/0'`, titled with its
  key origin such as `[73c5da0a/84'/0'/0']`. Native SegWit can be shown as
  either standard BIP32 `xpub` or, on request, its SLIP-132 `zpub` — the same
  78 bytes with the four version bytes swapped, since libwally itself
  serialises only the plain BIP32 versions and has no notion of SLIP-132.
  Taproot is `xpub` only: SLIP-132 defines no taproot version prefix, so there
  is no `zpub`-equivalent to offer it as;
- its mainnet receive addresses for indices 0 through 99 at `m/84'/0'/0'/0/i`
  or `m/86'/0'/0'/0/i`, browsed as a scrollable list of a hundred rows rather
  than stepped through one at a time. The whole list is derived when it is
  opened, the same up-front derivation the QR carousel below already relies
  on, so scrolling through it never re-runs BIP32.

Long values are paged three lines at a time in the 16px face, split by what
actually fits the display rather than by a character count, so a proportional
font can never drop a character from a value that is about to be transcribed.
The larger face costs four characters a line and a third fewer pages: it is 45%
taller than the small one but only 18% wider on base58. Footers keep the small face,
where 16px would run off the bottom of the display, and so do the keys still
spelled with a word: `OK` at 16px is 24px wide in a 24px cell. Backspace is drawn
as an arrow instead of spelled, since the fonts are 95 printable ASCII characters
and have no glyph for it.

The account key just shown — as `xpub` or `zpub`, whichever — and the address
last opened from its list (index 0 until one has been) can be shown as QR
codes. The QR screen steps sideways between the two, each one named in the
margin beside it, so an account key and the address it belongs to are one press
apart. An account key is encoded together with its key origin, as
`[73c5da0a/84'/0'/0']xpub...`, which is what a watch-only wallet needs to
import the account without being told the derivation path; 131 of the 134
bytes a version-6 code holds. There is nothing animated to scan: it is one
image.

**Photographing an account key QR reveals every address of that account, past and
future.** That is the whole point of the code and the whole cost of it, and the
device says so before showing one. A passphrase is never encoded as QR under any
circumstance. A mnemonic can be, but only as a deliberate, separately-warned
opt-in from the wallet viewer's Backup menu — see Backup export below — never as
a side effect of anything else the device shows.

## Backup export

Removing a code path and warning before using one are both tools this project
reaches for, chosen by what the risk actually is. The RNG-to-seed path above
has no legitimate reason to exist at all, so it doesn't. Exporting a mnemonic
has one — moving it to another wallet, or onto a metal plate — the same trade
the account-key QR above already makes, so instead of removing it the device
warns as sternly as the stakes call for and asks for a deliberate choice every
time rather than trusting a warning read once.

From the wallet viewer's `Backup` menu, a generated or restored mnemonic — the
same screen `New Seed` and `Restore Seed` both end on — can be shown two more
ways, each read-only and each requiring its own acknowledgement before anything
is drawn.

**Stackbit 1248** shows one word per screen as its one-based word number
(`abandon`=1, `zoo`=2048, the same convention word-number restore already uses)
split into four decimal digits, punched as binary weights 1, 2, 4 and 8 — what
a Stackbit 1248 metal plate is punched to record. Choosing it first asks which
layout to draw:

- **Simple grid**: four columns, one per digit, four rows, one per weight —
  easier to read at arm's length and the one to use if nothing needs to match
  the plate's own printed layout by eye.
- **Physical layout**: the plate's own arrangement (github.com/selfcustody/krux,
  src/krux/pages/stack_1248.py: `_draw_grid`/`_draw_punched`) — two rows, not
  four. The thousands digit gets one column of two cells (weight 1 on top,
  weight 2 below — the digit is never more than 2, so only one is ever lit);
  the other three digits each get a 2x2 block, top-left=1, top-right=2,
  bottom-left=4, bottom-right=8. For punching a plate side by side with the
  screen.

Both encode the same word the same way; only where each punch is drawn
differs. `L/R` step word to word, `BOTH` returns to the Backup menu. This is a
display only: a plate already punched from a Stackbit-compatible device
restores today with zero new code, by choosing "Enter word numbers" during
Restore Seed and typing what is punched. Stackbit 1248 is a third-party
physical backup product; Origo has no affiliation with it.

**Compact SeedQR** encodes the mnemonic's raw entropy directly as a byte-mode
QR — 16 bytes for 12 words, 32 for 24 — with no checksum bits and no word text,
the SeedSigner/Krux "Compact SeedQR" convention. Unlike the account-key and
address QR codes above, which are always drawn at the fixed version that holds
a key origin and an xpub, this code is drawn at the smallest QR version that
holds its entropy — version 1 for 16 bytes, version 2 for 32 — matching the
"compact" of the convention's name rather than padding it out with meaningless
filler modules. Before anything is drawn, the
device warns that this single code is the entire seed and that a photograph of
it is total, irreversible loss of every fund it can ever control — a materially
different warning from the account-key QR's, which only ever exposes addresses.
The device has no camera, so unlike Jade — which gates mnemonic QR export behind
camera-equipped hardware specifically so it can scan the result back and check
it — Origo cannot verify its own render; independently confirm the payload
instead with `tools/origo_verify.py inspect`, which prints the same entropy
bytes this code encodes. Origo implements only this compact/binary form: not
Standard (numeric) SeedQR, Plaintext QR or Encrypted QR.

## Run on a PC

The SDL simulator runs the firmware's actual application flow, deterministic
core, renderer, fonts, QR encoder and pinned libwally source. Only the display,
buttons, clock and secp256k1 blinding randomness are replaced by host adapters.

Everything below the framebuffer is therefore outside its reach: the SPI path,
the panel's initialisation and the byte order pixels are sent in are exercised
only on the device. A colour that is right in the simulator can still be wrong on
the board, and once was. What can be pinned from the host is pinned — the wire
byte order has its own check in the self-test, and another test asserts the
driver still calls it — but a screen is finally judged on the hardware.

Install the native build dependencies and run it:

```sh
# Fedora
sudo dnf install gcc cmake pkgconf-pkg-config SDL2-devel

# Ubuntu/Debian
sudo apt install build-essential cmake pkg-config libsdl2-dev

./tools/run-simulator.sh
```

Use `Left` or `A` for the left TTGO button and `Right` or `D` for the right one.
Select with `Enter` or `Space`, or by holding one arrow and pressing the other
as you would on the device. `Q` or `Escape` closes the simulator. Run the
non-graphical compiled-core check with `./tools/run-simulator.sh --self-test`;
it verifies the published BIP84/BIP86 address and account xpub vectors, the
published SLIP-132 zpub vector for the same BIP84 account and that BIP86
rejects a zpub request outright, that every prefix of every BIP39 word and all
2048 word numbers are reachable through their keyboards, that every one of
those 2048 words round-trips back to the same one-based number, that paged text
reassembles byte for byte, that a scrolling list always keeps the selection on
screen and never pads its end with blank rows for lists as large as the
hundred-row address browser, that the rearranged keyboards still hold every
letter and every printable character, that an account key with its origin
still fits a single QR code, that every one of the 2048 Stackbit 1248 punch
grids lights exactly the cells its digits' bits call for in both the simple and
the physical layout, and that the Compact SeedQR payload for the published
12- and 24-word zero-entropy vectors is exactly their raw entropy, drawn at
QR version 1 and 2 respectively rather than the larger fixed version the
account-key and address QR codes use.

The simulator is for development with published test vectors. Do not enter a
real mnemonic, passphrase or entropy transcript on a network-connected PC.

## Build and flash

Clone the repository and its pinned libwally dependency, then build with
ESP-IDF 5.5.4:

```sh
git clone --recurse-submodules git@github.com:oroderico/origo.git
cd origo
source /path/to/esp-idf-v5.5.4/export.sh
idf.py -B "$PWD/build" -D SDKCONFIG="$PWD/build/sdkconfig" build
sha256sum build/origo.bin
python3 tools/audit_origo_elf.py build/origo.elf \
  --map build/origo.map \
  --bin build/origo.bin \
  --nm xtensa-esp32-elf-nm
idf.py -B "$PWD/build" -p /dev/ttyUSB0 flash
```

The application component contains only the deterministic core, TTGO display
driver, two fonts and a version-6 QR encoder. Its libwally component is built
without Elements. The audit rejects linked wallet, radio, persistence, OTA update,
battery, generic graphics, transaction, PSBT and Elements symbols and enforces
a 295 KiB image limit. LEDC is the one peripheral driver let through that list,
solely for the backlight's brightness PWM - it carries no wallet, radio or
persistence surface of its own. The partition table contains only the factory
application: there is no NVS, PHY-data or OTA slot.

## Independent verification

`tools/origo_verify.py` is dependency-free and independently implements
BIP39, BIP32, BIP84, BIP86 and Bech32/Bech32m. Examples:

```sh
python3 tools/origo_verify.py generate d20 --words 12 1 2 3 ...
python3 tools/origo_verify.py complete "abandon ... abandon" 0000000
python3 tools/origo_verify.py inspect "abandon ... about" --index 0
```

`inspect` prints the checksum verdict, one-based word numbers, the Compact
SeedQR payload, master fingerprint, both account xpubs, the exact payload of
both account key QR codes and both addresses, so every value the device can
display has an independent second implementation to be compared against.

Do not type a real mnemonic or passphrase into a network-connected computer.
Boot a trusted offline environment, verify this repository and tool first, and
compare the transcript, full hash, fingerprint, xpubs, and addresses.

## Safety boundaries

This tool protects against a compromised hardware RNG only when the human
entropy source and recording process are sound. It cannot protect against a
compromised display, malicious firmware, biased physical dice/cards/coins,
shoulder surfing, mistakes copying entropy, or a Compact SeedQR photographed
by anyone other than the person who asked to see it. Reproduce the result on a
second independent implementation before funding an address. The ten-minute
inactivity timer gives a 60-second extend-or-erase warning; cancel, error,
timeout, and restart paths wipe session buffers before returning or rebooting.
