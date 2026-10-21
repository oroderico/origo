# Entropy

How dice, coins and cards become a mnemonic, and how the device grades them.

## Entropy transcripts

### Recording one

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
| D6 | 60 rolls | 120 rolls | digits concatenated, e.g. `123456` |
| D20 | 36 rolls | 68 rolls | decimal rolls joined by `-`, e.g. `1-20-7` |
| Coins | 128 flips | 256 flips | Heads=`1`, Tails=`0`, concatenated |
| Cards | first 25 distinct cards | first 50 cards, with replacement | `cards-v1:` plus rank/suit codes |

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

### From transcript to mnemonic

This is what every source does, and what `Coin flips` does under **Flip and
hash**. The alternative for coins, which skips the hash so the arithmetic can
be checked by hand, is [described below](#flipping-words-directly).

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
than guessing. Those flips are the missing entropy bits and lead to one
checksum-valid final word: the firmware never randomly selects from the 128 or
8 otherwise-valid endings.

Neither path ever reads the device RNG. The entropy quality bar described
next grades what has already been typed; it cannot add or remove a single bit
from what gets hashed.

### Flipping words directly

`Coin flips` offers that same unhashed arithmetic as a second method, **Flip
each word**, reached by flipping the words rather than typing them. It exists
because coins are the one source a person can convert to BIP39 words by hand,
and hashing throws that away: re-entering a transcript elsewhere reproduces the
mnemonic, but nobody checks on paper that word 7 is the word their coin
produced, because checking it means computing SHA256 of a 128-character string.

The wordlist is exactly 2048 long, so eleven flips name one word — every word
reachable, none favoured, no rejection sampling and no modulo skew. Heads is 1,
tails is 0, most significant bit first:

```
00000000000  ->  0001  ->  abandon
01100110011  ->  0820  ->  grid
11111111111  ->  2048  ->  zoo
```

Eleven such words are 121 bits, so seven loose flips finish the 128 a 12-word
mnemonic needs and BIP39's checksum supplies the twelfth word; 24 words is
twenty-three words and three loose flips for 256. The flip count is identical
to the hashed method's — 128 or 256 — so neither method asks for more coin
tosses than the other, and neither has more entropy behind it.

The final word is the one step a wordlist alone cannot do, since part of it is
the checksum. That is the same arithmetic `Complete checksum` above performs,
reached from a different direction.

There is no quality bar on this path, deliberately: eleven flips are eleven
bits by construction, nothing is being estimated, and a gate could only
second-guess the reader's coin. The hashed method keeps its bar unchanged.

`tools/origo_verify.py complete` checks the result — convert each group of
eleven flips against a printed wordlist, then hand it those words and the loose
flips.

### SeedPicker tickets

[SeedPicker](https://seedpicker.net) draws each word by hand from 342 paper
tickets, T1 to T342, and one six-sided die. Origo is not affiliated with it;
it takes the method, not any code. Print SeedPicker's ticket cutouts and lookup
table, cut the tickets out into a bag, then for each word draw a ticket, put it
back, mix, and roll the die. The pair names word number

```
(ticket - 1) x 6 + die
```

so T1 with a 1 is `0001 abandon` and T342 with a 2 is `2048 zoo`. 342 tickets
and six faces make 2052 pairs: the 2048 words once each, and four left over —
T342 with a 3, 4, 5 or 6 — which SeedPicker's table answers with "Draw a new
ticket". Origo does the same and says so. Drawing a new *ticket* matters:
rolling only the die again would make T342's two words three times as likely
as any other. Because the ticket goes back every time, every word is equally
likely and independent of the rest, eleven bits each, and SeedPicker's
printed table is all it takes to check one.

On the device it is `SeedPicker tickets` in the entropy source list. Each word
asks for the ticket on the number keypad, which offers only the digits that
still lead to a ticket from T1 to T342, and the die on a carousel, then shows
the pair, the number and the word together — `T57 + die 4`, `0340  clerk` —
to be checked against the table before it is used. After 11 or 23 words, 7 or 3
loose coin flips and BIP39's checksum finish the mnemonic, exactly as **Flip
each word** does; there is no hash and no quality bar.

SeedPicker itself only makes 24 words, and its last-word calculator always
takes the alphabetically first valid 24th word. That is the one whose three
loose bits are zero, because the wordlist is alphabetical and those bits lead
the last word's index: three tails on Origo give SeedPicker's own result.
Flipping them instead gives the full 256 bits rather than 253.

A worked example, 24 words:

| | | | |
|---|---|---|---|
| T57+4 → 0340 clerk | T201+1 → 1201 noodle | T342+2 → 2048 zoo | T1+1 → 0001 abandon |
| T118+6 → 0708 flash | T300+3 → 1797 they | T23+5 → 0137 baby | T171+2 → 1022 leisure |
| T256+4 → 1534 sausage | T89+6 → 0534 dress | T14+3 → 0081 anxiety | T330+1 → 1975 walnut |
| T77+2 → 0458 decrease | T142+5 → 0851 heart | T5+6 → 0030 admit | T268+1 → 1603 sign |
| T199+3 → 1191 network | T36+4 → 0214 boy | T311+2 → 1862 trim | T160+6 → 0960 jewel |
| T44+1 → 0259 cake | T228+5 → 1367 print | T97+3 → 0579 embark | |

Three tails finish it with `cabin`, master fingerprint `60c2d222`, which is
what SeedPicker's calculator gives for those 23 words; heads, tails, heads
gives `phrase` instead. `tools/origo_verify.py tickets` reproduces it from the
draws:

```sh
python3 tools/origo_verify.py tickets 57:4 201:1 342:2 ... 97:3 --bits 000
```

### The quality bar

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
small (empirically under 1%, at the 50 draws that mode asks for) chance of
an honest run tripping "poor entropy" once. Adapted from Krux's dice-roll
entropy screen (github.com/selfcustody/krux).

### How the counts were chosen

Dice and card counts are set so that an honest run does not merely pass the
gate but reads at or above the minimum on screen: D6 at its old 50 and 99
rolls could carry at most 129.2 and 255.9 bits, so on a simulation of 20000
honest runs 21.9% and 36.6% respectively reported fewer bits than the seed
needs. Almost all still generated, absorbed by the four-bit tolerance, but a
tool whose whole claim is not overstating entropy should not routinely show a
number that reads short. Coin flips are the exception that stays, and the
figure is published rather than buried: about a quarter of honest coin runs
still report a bit short, at both lengths. One flip is exactly one bit, so 128
and 256 flips are simultaneously the theoretical minimum and the whole of the
transcript buffer — padding them would mean widening a field inside the
generated-seed struct, which is a larger change than the counts above.

These counts are therefore no longer Krux's, which is where they started:
Krux asks for at least 50 and 99 D6 rolls, or 30 and 60 D20, and Origo now
asks for 60/120 and 36/68. Krux's own source calls its minimum a count that
"hardly will reach min. entropy according to Shannon's index" and absorbs
that two ways Origo cannot — its counts are floors the user may keep rolling
past, and it grades with a 2-bit tolerance. A fixed-length run has neither
escape, so Origo buys the same margin in rolls. The consequence is that a
seed generated on Krux at its own minimum cannot be reproduced here by
entering the same rolls; the transcript format, hash and BIP39 encoding are
unchanged, so a Krux run of matching length still reproduces exactly.
