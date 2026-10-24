# Contributing

Origo does one thing — turn hand-recorded dice, coins or tickets into a
BIP39 mnemonic, a word at a time, deterministically and without touching the
device RNG — and its value is being small enough to audit in full. Read
[docs/why.md](docs/why.md) before proposing anything: every change here gets
judged against the threat model described there, not against how useful or
interesting it is in isolation.

## Before you start

For a small, self-contained bug fix, just open a pull request. For anything
bigger — a new entropy source, a new dependency, a new derivation path, a new
screen — open an issue first and describe the problem it solves. This project
turns down otherwise-reasonable features on scope grounds alone: anything
that isn't a pure function of what the user typed, that reintroduces a
device-side source of randomness into the seed path, or whose words cannot
each be checked against a printed table (a hash, an estimate), works against
the one property Origo exists to guarantee. Discussing it before writing code saves
you from a rewrite.

## Building and testing locally

Full setup instructions live in [docs/build.md](docs/build.md) — this just lists what a pull request is expected to pass, which is
exactly what CI (`.github/workflows/build.yml`) checks:

```sh
# The C tests (tests/test_*.c): published vectors, exhaustive wordlist and
# keyboard properties, entropy statistics, and what the screens draw.
cmake -S host -B build-host -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-host --parallel
ctest --test-dir build-host --output-on-failure

# The same tests instrumented: the out-of-bounds access and the undefined
# behaviour that reading the code did not think to look for.
cmake -S host -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DORIGO_SANITIZE=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure

# Independent Python re-implementation of the mnemonic/entropy math,
# plus the RNG-absence and pure-function checks.
python3 -m unittest discover -s tests -v

# Firmware build, twice, independently audited and then compared
# byte-for-byte: a non-reproducible build is treated as a failure.
idf.py -B build-a -D SDKCONFIG=build-a/sdkconfig build
idf.py -B build-b -D SDKCONFIG=build-b/sdkconfig build
cmp build-a/origo.bin build-b/origo.bin
python3 tools/audit_origo_elf.py build-a/origo.elf --map build-a/origo.map \
  --bin build-a/origo.bin --nm xtensa-esp32-elf-nm
```

A change that only touches host-testable code (most of `main/seedtool_core.c`,
the render logic) can be fully verified with the first three steps
and never needs a flashed board. A change to the app (`main/app_*.c`,
`main/seedtool_app.c`) that is not meant to change what the device shows can be
checked with `origo-replay`, which runs the app from a key script and prints a
hash of every frame: replay the same random scripts on the parent commit and on
yours, and the outputs must match. Given a directory as a second argument, it also
writes every frame there as a PPM, for a change that is meant to alter a screen. A change to hardware-facing code (display
driver, input, backlight) still needs a real TTGO T-Display: the simulator
deliberately does not reach the SPI path or the panel's byte order, and a
colour that is right on host has been wrong on hardware before.

## Code style

There is no `.clang-format`; match the file you are editing. Public functions,
types and macros use the `seedtool_` / `SEEDTOOL_` prefix; everything is
snake_case. Comments justify a non-obvious *why* — a bias correction, a buffer
size that must track a specific constant, a trade-off that was deliberately
made — never restate *what* the next line already says, and never narrate how
the code got here: that is what the commit message is for. `main/seedtool_core.c`
and its header are the clearest examples of the standard: read a few of their
comments before writing your own.

## Adding a dependency

Treat this as an exceptional change, not a routine one. `tools/audit_origo_elf.py`
enforces the image budget and the list of rejected symbols described in
[docs/build.md](docs/build.md#build-and-flash), and a dependency has to be
justified against that list, not just against whether it compiles. Origo has turned down a two-kilobyte size win before
because it required a decompressor the audit is built to reject (see the
logo section of [docs/verify.md](docs/verify.md)); expect the same bar to apply. A PR that adds a
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
mnemonic, passphrase, rolls or flips, private key or funded address
anywhere in one — see [SECURITY.md](SECURITY.md) for how to report instead.

## License

By contributing, you agree your changes are made available under this
repository's [MIT license](LICENSE).
