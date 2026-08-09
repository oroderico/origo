# Contributing

Origo does one thing — turn hand-recorded dice, coin or card entropy into a
BIP39 mnemonic, deterministically and without touching the device RNG — and
its value is being small enough to audit in full. Read the "Why" section of
[README.md](README.md) before proposing anything: every change here gets
judged against the threat model described there, not against how useful or
interesting it is in isolation.

## Before you start

For a small, self-contained bug fix, just open a pull request. For anything
bigger — a new entropy source, a new dependency, a new derivation path, a new
screen — open an issue first and describe the problem it solves. This project
turns down otherwise-reasonable features on scope grounds alone: anything
that isn't a pure function of what the user typed, or that reintroduces a
device-side source of randomness into the seed path, works against the one
property Origo exists to guarantee. Discussing it before writing code saves
you from a rewrite.

## Building and testing locally

Full setup instructions live in README.md's [Run on a
PC](README.md#run-on-a-pc) and [Build and flash](README.md#build-and-flash)
sections — this just lists what a pull request is expected to pass, which is
exactly what CI (`.github/workflows/build.yml`) checks:

```sh
# Compiled-core self-test: address/xpub vectors, keyboard reachability,
# paging, Stackbit grids, Compact SeedQR payloads, and more.
./tools/run-simulator.sh --self-test

# Independent Python re-implementation of the mnemonic/entropy math,
# plus the RNG-absence and pure-function checks.
python3 -m unittest discover -s tests -v

# QR encoder, built standalone with warnings as errors.
cc -std=c11 -Wall -Wextra -Werror -DLOCK_VERSION=5 -Imain \
  main/qrcode.c tests/qrcode_smoke.c -o /tmp/origo-qrcode-smoke
/tmp/origo-qrcode-smoke

# Firmware build, twice, independently audited and then compared
# byte-for-byte: a non-reproducible build is treated as a failure.
idf.py -B build-a -D SDKCONFIG=build-a/sdkconfig build
idf.py -B build-b -D SDKCONFIG=build-b/sdkconfig build
python3 tools/audit_origo_elf.py build-a/origo.elf --map build-a/origo.map \
  --bin build-a/origo.bin --nm xtensa-esp32-elf-nm
python3 tools/audit_origo_elf.py build-b/origo.elf --map build-b/origo.map \
  --bin build-b/origo.bin --nm xtensa-esp32-elf-nm
cmp build-a/origo.bin build-b/origo.bin
```

A change that only touches host-testable code (most of `main/seedtool_core.c`,
the render/transcript logic) can be fully verified with the first three steps
and never needs a flashed board. A change to hardware-facing code (display
driver, input, backlight) still needs a real TTGO T-Display: the simulator
deliberately does not reach the SPI path or the panel's byte order, and a
colour that is right on host has been wrong on hardware before.

## Code style

There is no `.clang-format`; match the file you are editing. Public functions,
types and macros use the `seedtool_` / `SEEDTOOL_` prefix; everything is
snake_case. Comments justify a non-obvious *why* — a bias correction, a buffer
size that must track a specific constant, a trade-off that was deliberately
made — never restate *what* the next line already says. `main/seedtool_core.c`
and its header are the clearest examples of the standard: read a few of their
comments before writing your own.

## Adding a dependency

Treat this as an exceptional change, not a routine one. `tools/audit_origo_elf.py`
enforces a 295 KiB firmware image and rejects linked wallet, radio, OTA,
persistence, battery, generic-graphics, transaction, PSBT and Elements symbols
by name — a dependency has to be justified against that list, not just against
whether it compiles. Origo has turned down a two-kilobyte size win before
because it required a decompressor the audit is built to reject (see
README.md's logo section); expect the same bar to apply. A PR that adds a
dependency should update `tools/audit_origo_elf.py` (if it legitimately needs
a new allowance) and `THIRD_PARTY_NOTICES.md` together with the code, not as
a follow-up.

## Commit messages and pull requests

Subject line in the imperative mood (`Fix the ...`, `Restart the ...`); a
`fix(scope):` prefix is fine but not required — this repo uses both. The body
should explain the problem the change fixes, not just describe the diff, and
should say how you verified it — which of the checks above you ran, and
whether you flashed real hardware. That closing "verified by" line is already
the norm in this repo's history; keep it going.

## Security issues

Do not open an issue or PR for a vulnerability, and never paste a real
mnemonic, passphrase, entropy transcript, private key or funded address
anywhere in one — see [SECURITY.md](SECURITY.md) for how to report instead.

## License

By contributing, you agree your changes are made available under this
repository's [MIT license](LICENSE).
