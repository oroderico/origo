# Entropy

How dice, coins and SeedPicker's tickets become a mnemonic, a word at a time,
each word checkable against a printed table.

**Print [the worksheet](worksheet.pdf)** before a run: the rules and the
faces-to-bits tables on one page, a grid to record the run on the next, then
all 2048 words with the coin flips, D6 rolls, D20 rolls and SeedPicker ticket
that make each, so any word is checked by eye with no arithmetic. Once a run is
written on it, the sheet is the seed.

## Every word from a table

The BIP39 wordlist is exactly 2048 long, so eleven bits name one word. Every
method on Origo makes those eleven bits directly from what the reader rolled,
flipped or drew, one word at a time, and shows the word it made before going
on. There is no hash between the dice and the words and no estimate of how
random they were: a word is the eleven bits it was built from, and a printed
wordlist and the method's own small table are all it takes to check it.

That is the rule the methods are held to, and the reason for every choice
below. A hashed transcript can be reproduced on another computer, but it cannot
be followed: nobody checks on paper that word 7 is what their dice produced,
because checking it means computing SHA256 of a long string. Here every word
can be checked as it is made, by hand.

The one step no table can do is the last word, part of which is BIP39's
checksum. After the words come 7 or 3 loose coin flips — the bits the last
word still needs — and the device computes the checksum from them, the same
arithmetic `Complete checksum` performs. The device RNG plays no part
anywhere: the mnemonic is a pure function of what the reader entered.

| Method | One word | 12 words | 24 words |
|---|---|---:|---:|
| Coin flips | 11 flips | 128 flips | 256 flips |
| D6 dice | 6 rolls of 1 to 4 | 66 rolls + 7 flips | 138 rolls + 3 flips |
| D20 dice | 3 rolls of 1 to 16 | 33 rolls + 7 flips | 69 rolls + 3 flips |
| SeedPicker tickets | a ticket and a die | 11 draws + 7 flips | 23 draws + 3 flips |

The roll counts are of rolls that count; the dice need more throws than that,
as the next sections explain.

## Coin flips

Eleven flips name one word — every word reachable, none favoured. Heads is 1,
tails is 0, most significant bit first:

```
00000000000  ->  0001  ->  abandon
01100110011  ->  0820  ->  grid
11111111111  ->  2048  ->  zoo
```

Eleven such words are 121 bits, so seven loose flips finish the 128 a 12-word
mnemonic needs and BIP39's checksum supplies the twelfth word; 24 words is
twenty-three words and three loose flips for 256. Coins are the simplest
method to check: the flips are the bits.

## Dice

A die is read as bits too, but six and twenty are not powers of two, and that
is the whole difficulty. Turning every face into bits would favour some: read
a D6 "modulo 4" and faces 1 and 2 would each come up twice as often as 3 and 4.
So only as many faces count as are a power of two, and the rest are rolled
again before anything is entered:

- **D6:** 1 to 4 count, two bits each; **roll 5 or 6 again**.
- **D20:** 1 to 16 count, four bits each; **roll 17 to 20 again**.

A roll that counts is then exactly as likely as any other, so every word is.
The rerolled faces cost throws, not quality: a D6 needs on average 1.5 throws
per roll that counts, a D20 1.25. The device's carousel offers only the faces
that count, and the run opens on that rule, so a 5 cannot be entered by
mistake.

Face f is the bits of f − 1, most significant first:

| D6 | bits | | D20 | bits | D20 | bits |
|---|---|---|---|---|---|---|
| 1 | 00 | | 1 | 0000 | 9 | 1000 |
| 2 | 01 | | 2 | 0001 | 10 | 1001 |
| 3 | 10 | | 3 | 0010 | 11 | 1010 |
| 4 | 11 | | 4 | 0011 | 12 | 1011 |
| | | | 5 | 0100 | 13 | 1100 |
| | | | 6 | 0101 | 14 | 1101 |
| | | | 7 | 0110 | 15 | 1110 |
| | | | 8 | 0111 | 16 | 1111 |

Eleven does not divide by two or four, so each word's **last roll keeps only
its first bits**. A D6 word is five rolls of two bits and one of one bit — on
that roll 1 or 2 is 0, 3 or 4 is 1. A D20 word is two rolls of four bits and
one of three, the first three of that roll's four. The rule is still "only
these faces count"; the last roll simply contributes less of what it rolled,
and every word stays equally likely.

```
D6   4, 2, 1, 3, 3, 2  ->  11 01 00 10 10 0  ->  1685  spoon
D20  16, 1, 10        ->  1111 0000 100     ->  1925  vacuum
```

## SeedPicker tickets

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
printed table is all it takes to check one. The ticket keypad offers only the
digits that still lead to a ticket from T1 to T342.

Draws already made on SeedPicker's paper form go in another way: **Complete
checksum** → `23 words + 3 coins` → `Enter word numbers`, typing each number
straight from the lookup table.

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
gives `phrase` instead.

## On the device

Every method runs the same way. It opens on its rule — how many entries, or
which faces count — and then, for each word:

1. the flips, rolls, or ticket and die, one at a time, each step undoable;
2. a confirmation showing what made the word, its number and the word itself —
   `4, 2, 1, 3, 3, 2`, `1685  spoon` — to check against the table before it is
   used. Declining throws that word's entries away and asks for them again.

Under each flip, roll and ticket a bar counts the bits taken so far against
the 128 or 256 the seed needs — `13/128 bits`. It is a count, not a grade:
every entry that counts is a whole number of bits (a flip one, a D6 roll two,
a D20 roll four, a ticket and its die eleven), so it says how far the run has
come and nothing about how fair the dice are. No device can tell that from a
hundred rolls: a sequence like `1 2 3 4 1 2 3 4` has perfectly even faces and
no randomness at all, which is why Origo estimates nothing.

After the last word, a review lists every word with what made it, four to a
page — `01. 4,2,1,3,3,2 spoon`, `01. T57+4 clerk` — to compare with the
paper in one pass; its arrow returns to the last word. Then the loose coin
flips, BIP39's checksum, and the finished mnemonic, read back and confirmed
like any other.

## Complete checksum

`Complete checksum` takes 11 or 23 words already known — from a metal plate,
SeedPicker's form or any other table — and 7 or 3 coin flips. The words' own
11-bit wordlist positions are packed back to back to become the entropy
directly, the flips fill the bits the last word still needs, and libwally's
`bip39_mnemonic_from_bytes` computes the real checksum and so the one final
word that makes it valid. The flips must be exactly as many bits as are
missing — one too few or too many and the call is rejected rather than
guessed — and the firmware never picks among the 128 or 8 otherwise-valid
endings on its own. Every method above ends in this same arithmetic.

## Checking a run

By hand, each word is a row of [the worksheet](worksheet.pdf): find the word
the device shows and read its row under the method's column. A D6 row ends in a
pair such as `1-2`, a D20 row in one such as `9-10`: the last roll keeps only
its first bits, so either face of the pair makes the word.

`tools/origo_verify.py` reproduces each method from what was written down,
word by word, printing each word's entries, number and word before the
finished mnemonic:

```sh
python3 tools/origo_verify.py coin-words 0110011001101100110011...    # 128 flips, or 256 with --words 24
python3 tools/origo_verify.py dice d6 4 2 1 3 3 2 ... --bits 1011001  # 66 or 138 rolls
python3 tools/origo_verify.py dice d20 16 1 10 ... --bits 1011001     # 33 or 69 rolls
python3 tools/origo_verify.py tickets 57:4 201:1 ... 97:3 --bits 000  # 11 or 23 draws
python3 tools/origo_verify.py complete "abandon ... abandon" 0000000
```
