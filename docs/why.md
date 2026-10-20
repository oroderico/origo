# Why Origo

What Origo is for, and the failure it is built to rule out.

## What it replaces

Almost everything a seed needs done to it is arithmetic. Turning dice into
words, finishing a checksum, deriving an account key, reading off the
addresses of a branch, working out which holes to punch in a metal plate —
none of it needs a network, a signing key, or a general-purpose computer. It
does need *somewhere to run*, and the usual answers each cost something:

- **By hand.** A mnemonic's final word is a checksum, so finishing a
  hand-rolled seed means a printed wordlist, a SHA256 you trust, and no
  arithmetic slips along the way. An account xpub is not reachable this way
  at all.
- **An offline PC.** A disk that remembers, a network stack one command away
  from up, and an operating system far too large to audit before you type a
  seed into it. "Offline" there is a claim about configuration, not about
  construction — and configuration is exactly what drifts.
- **An old phone.** The same, plus a baseband processor you cannot inspect
  and cannot prove is asleep.

Origo is a fourth answer: a development board costing about as much as lunch,
running firmware that does this arithmetic and has no facility for anything
else. It generates a 12- or 24-word mnemonic from dice, coins or cards;
finishes a checksum from 11 or 23 words already known; validates a mnemonic
you already have; shows that mnemonic's master fingerprint, its BIP84, BIP86,
BIP49 or BIP44 account key — as `xpub`, as a SLIP-132 `ypub` or `zpub` where one
is defined, or
as a QR code — its output descriptor, and the first ten thousand addresses of
its receive or change branch; and exports a backup as words, as word numbers, as a
Stackbit 1248 punch pattern, or as a Compact SeedQR.

The difference is not that Origo is careful where a PC is careless. It is
that the things you would be trusting a PC *not* to do — write to a disk,
bring up a radio, run code you never audited — are absent from the build
rather than switched off within it. There is no filesystem, no persistence
partition, and no networking component anywhere in the build graph, so no
configuration change can turn one back on; putting one back would mean
editing the component list, which is visible in a diff.

What you trade is trusting this firmware instead, and the repository is
arranged so that you do not have to take that on faith: the build is
byte-for-byte reproducible, the linked surface is checked by
`tools/audit_origo_elf.py` on every build, and any seed the device produces
can be recomputed independently with `tools/origo_verify.py`. Those three are
what to verify before trusting it with anything; [Build, flash and
simulate](build.md) and [Verifying Origo](verify.md) describe each.

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
flag to misroute in the first place: a mnemonic is a pure function of what the
human recorded by hand — dice, coins or cards — and the device RNG plays no
part in it. Two derivations turn that record into words,
both deterministic and neither touching the RNG: the transcript reduced with
SHA256, which every source uses, and — for coins only — the flips packed
straight into the entropy without a hash, which is the one path a reader can
check against a printed wordlist with no device at all. Both are described
in [Entropy](entropy.md). A firmware regression that silently swapped the entropy source behind
a device owner's back, the same shape of bug that cost Coldcard users tens of
millions of dollars, has nothing to act on here: stub the RNG out entirely
and Origo generates the identical mnemonic.
