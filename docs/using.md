# Using Origo

The controls, entering words and passphrases, the wallet viewer and backup export.

## Controls

The board has two buttons and no select button. In a list, the left button
moves to the previous item and the right button to the next; both pressed
together select. Holding one button repeats it. A press is acted on only once
both buttons are released, so a chord is never mistaken for a step. A numeric
carousel (a die's face, a word number's digit) reads the same two buttons the
other way: left raises the value, right lowers it, since there the physical button read as
"up" raising what is on screen matters more than reusing a list's sense of
"previous".

Pressing a button teaches left, right and hold. It cannot teach the chord, since
nothing moves until both buttons are released, so `Up/Down move   BOTH select`
sits under the first screens reachable from the main menu and disappears for
good once the chord has been used once — not under the main menu itself, which
is not the place to also be teaching it. What stays under a screen after that is
its position counter alone, or nothing at all on a screen that answers with the
tick in the header rather than a bar along the bottom. The one footer that still
names a consequence rather than a gesture — the timeout's `Press any button to
stay` — is always shown, because missing it costs a session.

### Dice, coins and tickets

A die's face is set on a carousel that holds only the faces that count — 1 to
4 on a D6, 1 to 16 on a D20 ([Entropy](entropy.md#dice) says why) — so a face
to be rolled again cannot be entered at all.

Coin flips are a direct choice rather than a carousel position: left picks
Heads, right picks Tails, one press per flip. With a run as long as 128 or 256
flips, halving the presses per flip halves the whole entry. Both buttons
together undoes the last flip, since there is no longer a neutral carousel
position to step onto for that.

A SeedPicker ticket is typed on the number keypad, which darkens every digit
that would take the number past T342, then its die is set on a carousel.

### Going back

Three gestures is all two buttons afford, and all three are spoken for, so going
back cannot be a button: it is a place on the screen. Every screen with a stage
above it carries a back arrow at the left of its title bar, and it steps back
exactly one stage rather than abandoning the flow. On the word keyboard,
backspace deletes a letter; deleting past the start of a word returns to the
previous word, and past the first word to the menu before it. The numeric
carousel carries `[back]` one step below its lowest value, so a misread roll
is corrected by stepping back to it rather than by waiting out the session
timeout and starting the run again. Coin flips are the one
exception: both a value and its confirmation used to cost two of the three
gestures, so freeing the one that used to confirm turns it into "undo" instead
of a screen position, since a direct choice has nothing left to confirm. Neither of
these is visible until it is used, so the entry screens say where they are in
a footer that stays: `BOTH take   back below 1` on a carousel, `BOTH undoes
the last flip` on a coin.

### Settings

`Settings`, in the main menu and in a loaded wallet's menu (where leaving for
the main menu would erase the seed), carries `About` (the firmware's version -
the source's `git describe`, with `-dirty` if it was built from uncommitted
changes - then the safety disclaimer),
`Flip screen`, which turns the panel 180 degrees in place,
`Brightness`, which steps the backlight through five PWM levels, applied live
as it is adjusted, and `Keyboard`, alphabetical or QWERTY letters. All three
are session-only - like every other piece of UI state, they reset to their
defaults (unflipped, brightness 3 of 5, alphabetical) on the next boot, since
Origo has nothing to save them to.

### Lists and scrolling

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

The way out of a list is the arrow in its title bar, in the same place on every
screen. Two lists have none: the main menu, which has nothing above it, and the
wallet, whose only exit is its `Erase and restart` row, kept a row the reader
travels to because leaving erases the session.

## Entering words and passphrases

Entering a mnemonic asks first whether the words will be typed as letters or as
word numbers, and every word of that mnemonic is then entered the chosen way.

Typed as letters, only letters that still lead to a BIP39 word are reachable, and
once ten or fewer words remain they are listed outright. Typed as numbers, only
digits that still lead to a word number are reachable, and the number is shown as
the word it means, to be confirmed, before the next word is asked for. In both
cases deleting past the start of a word steps back to the previous one.

Restoring a mnemonic narrows the **last** word further still. That word carries
the checksum, so most of the wordlist cannot end a given eleven or twenty-three:
its eleven bits are the leftover entropy bits followed by the checksum bits,
which leaves 128 of the 2048 words possible for a 12-word mnemonic and only 8
for a 24-word one. The keyboard offers those and nothing else — so a word misread
off a metal plate is not typeable in the first place, rather than being refused
after the whole mnemonic has been entered. Few enough remain for a 24-word
restore that entry lists them outright instead of asking for a letter. The
narrowing is exact in both directions: everything it excludes genuinely fails
validation, and no correct seed becomes harder to enter. It
applies to both entry methods and to the last word on the review screen, where
it is rebuilt from the words currently entered — but not to "Complete checksum",
whose 11 or 23 words have no checksum in them yet, and not to the backup quiz,
which asks what the reader wrote down.

A word number is one-based: the position in a printed BIP39 English wordlist,
where `abandon` is 1 and `zoo` is 2048. It is one more than the zero-based index
the encoding itself uses, so a list numbered from zero must be read with that in
mind. `tools/origo_verify.py inspect` prints the same one-based numbers for a
mnemonic, and the self-test requires all 2048 of them to be typeable both plainly
and padded to four digits.

### The keyboards

The letter keyboards are alphabetical by default, in Jade's rows (`a`–`j`,
`k`–`t`, `u`–`z`): on a ring walked with two buttons, a reader who does not
touch-type finds a letter by where it falls in the alphabet. `Keyboard: QWERTY`
in Settings switches them. Settings opens only from a menu, never in the middle
of a flow, and a passphrase is typed and confirmed in one sitting, so it is
never typed on one arrangement and confirmed on the other. The number keyboard has one order. The
cursor opens on the middle key of the middle row — `o` on the alphabetical
letters, `g` on QWERTY, `5` on the numbers — because on a ring of thirty
keys walked with two buttons the corner is the furthest possible place to start
from. It then keeps its place, so repeating a character is one press rather than
a walk back, and returns to the centre when a keyboard is opened or a symbol page
is turned. When the letter it was resting on stops leading to a word, it moves to
the nearest key that still does.

The suggestion order, the initially selected key and where a list has scrolled to
are pure functions of what has been typed and chosen. None is randomised: no
screen in the entry path may depend on the device RNG.

### Passphrase

An optional printable-ASCII passphrase of at most 100 characters is typed on a
four-page keyboard covering the whole printable range, read back whole with its
length (the one sign of a space at either end), entered a second time, and exists
for that derivation session only. Backspace on an empty field steps back, so the
review returns to the keyboard with the text still there, and a second entry that
differs is retyped against the first rather than costing both. It is set from the wallet viewer's Derivation
screen rather than asked for on the way in: a session begins with none, and the
screen states which of the two is in force rather than leaving it to be assumed.
Changing or clearing it re-derives the master fingerprint, since that
fingerprint is a function of it — so the fingerprint is also the check that the
passphrase in force is the intended one.

## Restore and inspect

Existing mnemonics are validated before derivation; a bad checksum blocks the
address viewer entirely. For a valid mnemonic the viewer's own menu is titled
with the master fingerprint — `Wallet @73c5da0a`, the fingerprint in the body
weight and a light green of its own, apart from the orange that marks a
selection — rather than spending a row on showing it. It is an identity rather than an action, and being read without
being asked for is what makes it a check: it is a function of the passphrase in
force and moves the moment that does, so a reader who knows their wallet's
fingerprint sees at a glance whether the device is deriving that wallet.

A **Derivation** screen holds the three things that decide what everything else
derives, ordered by how deep each one cuts: the optional passphrase, which
decides the seed itself and so every key the device can produce; the wallet
type — Native SegWit (BIP84), Taproot (BIP86), Nested SegWit (BIP49) or Legacy
(BIP44), the last two for checking and restoring older wallets — which picks a
path from that
seed; and the account index (`m/type'/0'/account'`, 0 through 999), which picks
a branch of that path. All three last for the whole viewing session rather than
one visit to a screen, so checking account 2 under both types means setting it
once. The passphrase
can be changed or removed at any point without leaving the session, its own row
says which of the two is in force, and the fingerprint in the title is
re-derived whenever it changes. One type is in force at a time, so an
account key and its addresses are always read for the type currently set
rather than interleaved with the other one's — the derivation path shown with
every value names it, so what is on screen says which type produced it rather
than leaving it to be remembered. The wallet menu itself then offers, for
whatever is set:

- **Extended public key**, holding every way the account's watch-only key at
  `m/84'/0'/0'`, `m/86'/0'/0'`, `m/49'/0'/0'` or `m/44'/0'/0'` leaves the device — they are the same 78 bytes
  three ways, so they sit together rather than as separate rows that look
  unrelated while carrying identical risk. As `xpub`, titled with its key
  origin such as `[73c5da0a/84'/0'/0']`; as SLIP-132's `zpub` (Native SegWit)
  or `ypub` (Nested SegWit), the same 78 bytes with the four version bytes
  swapped, since libwally serialises only the plain BIP32 versions and has no
  notion of SLIP-132 — SLIP-132 defines no prefix for taproot or legacy, and
  under those types that row is absent rather than present and refusing; or as
  the output descriptor
  described below;
- its mainnet addresses for indices 0 through 9999, on whichever branch is asked
  for: `Addresses` opens a Receive/Change choice first, and the list that
  follows is `m/84'/0'/0'/0/i`, `m/86'/0'/0'/0/i`, `m/49'/0'/0'/0/i` or
  `m/44'/0'/0'/0/i` for receive,
  `.../1/i` for change, with each address titled by its own full path so the
  branch is never inferred from which menu it was reached through. Each row
  keeps both ends of its address, `0 bc1qcr8te4...306fyu`: after the `bc1q`
  every address of the type shares, six characters of the start and six of the
  end on every row, since an address is checked by its start and end and one
  made to look alike copies both. The font is proportional, so about one bech32
  row in 3,400, crowded with wide letters, keeps five and five instead. The
  address's own screen shows it whole. The list
  shows the first fifty; a "Go to index" entry at the end of it takes any
  index in range on a four-digit keypad, since scrolling five rows at a time
  to reach index 87 is not browsing, and a wallet used for years passes a
  hundred. The fifty are derived when the screen is opened and stay derived
  until it is left, so neither scrolling the list nor stepping back to it from
  an address re-runs BIP32; an index past them is derived on its own when it
  is asked for. One branch is listed at a time: switching between them
  re-derives, which costs what changing the account already costs. The chain step
  sits below the account key, so only the cheap half of the derivation
  actually repeats;
- its BIP380 output descriptor, the third entry under Extended public key —
  script type, key origin, account key and an
  8-character checksum in the one string a watch-only wallet imports directly,
  as `wpkh([73c5da0a/84'/0'/0']xpub.../<0;1>/*)#hpg6d6w2` or its `tr()`,
  `sh(wpkh())` or `pkh()` equivalent. The chain step is BIP389's multipath `<0;1>`, so the single
  descriptor describes the receive and the change branch together: a wallet
  imported from a receive-only descriptor has nowhere to put its change. That
  is a deliberate trade rather than a free upgrade — BIP389 is newer than
  BIP380, and a wallet that predates it (Bitcoin Core before v26.0, among
  others) rejects the `<0;1>` outright rather than reading half of it. Such a
  wallet wants the receive-only form, `.../0/*`, which is the same string with
  the multipath step replaced and its checksum recomputed; `tools/origo_verify.py`
  is the offline way to produce one. Always plain `xpub`, never `ypub` or `zpub` —
  SLIP-132's version bytes are a display convention BIP380 has no notion of. It
  carries the same account key the QR does, so it reveals every address of the
  account exactly as that does, and the device says so before showing one.

Long values are paged three lines at a time in the 16px face, split by what
actually fits the display rather than by a character count, so a proportional
font can never drop a character from a value that is about to be transcribed.
The larger face costs four characters a line and a third fewer pages: it is 45%
taller than the small one but only 18% wider on base58. Footers keep the small face,
where 16px would run off the bottom of the display, and so do the keys still
spelled with a word: `OK` at 16px is 24px wide in a 24px cell. Backspace is drawn
as an arrow instead of spelled, since the fonts are 95 printable ASCII characters
and have no glyph for it.

The account key just shown — as `xpub`, `ypub` or `zpub`, whichever — and the address
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
reaches for, chosen by what the risk actually is. The RNG-to-seed path
([Why Origo](why.md#why)) has no legitimate reason to exist at all, so it doesn't. Exporting a mnemonic
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
differs. `L/R` step through the words, which sit on the same ring the paged
screens use: the back arrow is the position above the first word, so stepping
off either end lands on the way out rather than wrapping past it. `BOTH` reads
on to the next word, and returns to the Backup menu when the arrow is the thing
selected. This is a display only: a plate already punched from a Stackbit-compatible device
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
