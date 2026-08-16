---
name: audit
description: Audit this Origo firmware tree for quality, quantity and security. Runs the project's own gates - host self-test, Python tests, QR smoke test, sanitized build, firmware build and ELF audit - then reports the numbers they emit and examines the invariants none of them cover. Use when asked to audit, review the health of, or check the state of the repository, before a release, or after a run of changes. Not a substitute for /code-review or /security-review, which cover generic ground this deliberately does not repeat.
---

# Auditing Origo

Origo carries more self-verification than most projects its size: an ELF audit
that proves the linked surface, a host self-test with thirty-odd named checks,
a Python suite that includes structural greps of the C, a sanitized build, a
fuzzer, and a byte-for-byte reproducibility check. All of it is pass/fail and
none of it reports a trend.

This audit exists for the space around those gates. Its value is not in
re-proving what they already prove — it is in reading the numbers they emit
before those numbers become failures, and in looking at the claims the project
makes about itself that nothing mechanical checks.

## Ground rules

**Never re-derive what a tool already proves.** Run the tool, read its output,
move on. Time spent hand-checking something `audit_origo_elf.py` already
guarantees is time not spent on the parts nothing guarantees.

**Exclude `jade/` from every repo-wide search.** It is an untracked reference
copy of Blockstream Jade — over a thousand files that will drown any grep and
produce findings about code that is not part of this project. Use
`--exclude-dir=jade` or restrict paths to `main/ host/ tools/ tests/`.

**Separate measured from inferred.** A number read from a tool's output and a
conclusion drawn by reading code are different kinds of claim, and the report
should not blur them. Cite `file:line` for anything found by reading.

**Report what is unproven, not just what is wrong.** "This invariant is stated
in the README and nothing enforces it" is a finding. Say what would enforce it.

## Stage 1 — run the gates

Cheapest first, so a failure surfaces before an expensive build.

```sh
./tools/run-simulator.sh --self-test
python3 -m unittest discover -s tests
cc -std=c11 -Wall -Wextra -Werror -Imain main/qrcode.c tests/qrcode_smoke.c \
  -o /tmp/origo-qrcode-smoke && /tmp/origo-qrcode-smoke
cmake -S host -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DORIGO_SANITIZE=ON \
  && cmake --build build-sanitize --parallel \
  && ./build-sanitize/origo-simulator --self-test
```

`unittest discover` rather than pytest: it is what CI runs, and the audit
should fail the way CI fails.

Then the firmware build and ELF audit. **Use the pinned IDF**, not whatever
`idf.py` is on PATH — `dependencies.lock` names the version
(`idf: version:`, 5.5.4 as of writing); find or install an export script for
that exact version rather than assuming one is at any particular path, since
where IDF installs lives is a per-machine fact this skill cannot pin. Whatever
`idf.py` resolves to by default may be a different version entirely - building
with it fails a toolchain check before reaching any project code, which is an
environment finding to report, not a code finding, and not something to "fix"
by letting the lock file be rewritten:

```sh
bash -c 'source <path to the pinned IDF>/export.sh >/dev/null 2>&1 && cd <repo root> \
  && idf.py build \
  && python3 tools/audit_origo_elf.py build/origo.elf --map build/origo.map \
       --bin build/origo.bin --nm xtensa-esp32-elf-nm'
```

Report each gate as pass or fail with the line that says so. A failure here
ends the audit: everything below assumes a tree that builds.

## Stage 2 — quantity

### Firmware and memory

The ELF audit prints the binary size on success and fails only at the ceiling
(`--max-bin-size`, default 512 KiB = 524,288 bytes, `tools/audit_origo_elf.py`).
Between those two states it says nothing, so report the headroom explicitly:

- bytes, and percentage of the ceiling
- bytes remaining
- the delta against the previous commit's binary when one is available

A build sitting above 95% of the ceiling is a finding in its own right, even
while it passes: the project has already refused a feature on size grounds
(deflate, ~2 KiB, rejected because the decompressor is a component the audit
forbids), so remaining space is a design constraint and not just a number.

Then measure what nothing measures:

```sh
bash -c 'source <path to the pinned IDF>/export.sh >/dev/null 2>&1 && cd <repo root> \
  && idf.py size && idf.py size-components'
```

`.bss`, `.data` and IRAM have no budget and no automated report anywhere, yet
`main/seedtool_app.c` reasons carefully about `.bss` cost in several comments.
Record the section breakdown so successive audits can compare it.

### Check coverage

Inventory what actually enforces what, and where the gaps are:

- `_Static_assert` sites in `main/` — compile-time invariants
- source-grep pins in `tests/test_origo_verify.py` — invariants a compiler
  cannot see, pinned by asserting on the C source text
- named checks in `host/origo_simulator.c`'s `self_test()`

Then the part that matters: **claims the project states in prose that nothing
checks.** Read README's Safety boundaries, `SECURITY.md`, `CONTRIBUTING.md` and
the header comments in `main/`, and for each concrete claim ask what would fail
if it stopped being true. Known members of this set, to re-check rather than
rediscover:

- "every screen that holds seed material wipes its buffers when it is left"
- the partition table having no NVS, PHY-data or OTA slot
- `CONFIG_ESP_WIFI_ENABLED` / `CONFIG_BT_ENABLED` being absent from the
  generated `sdkconfig` entirely, rather than set to `n`
- the logo being "the only picture in the firmware"
- the RNG being stubbable without changing any generated mnemonic

### Code volume

Lines per file, and the largest functions. `main/seedtool_app.c` holds every
screen and is the file that grows; report concentration and growth rather than
absolute size, which on its own means little. A function that has doubled since
the last audit is worth a look even if it is still short.

## Stage 3 — security

Origo-specific. Generic vulnerability classes belong to `/security-review`.

**Secret wiping.** The stated boundary is that every screen holding seed
material wipes its buffers on the way out, and `seedtool_zero` is called around
a hundred times to that end — with no test verifying a single one of them. For
each buffer holding a mnemonic, passphrase, seed or raw entropy, trace every
exit: success, cancel, error, and timeout. The error and timeout paths are
where real gaps have been found before, twice, so weight them accordingly. A
buffer in `.bss` deserves more suspicion than one on the stack, since it
persists.

**RNG confinement.** `tests/test_origo_verify.py` greps three files for RNG
calls and proves absence there. Check that no new path reaches the RNG, and
note that no test actually stubs the RNG and re-derives a mnemonic — which is
precisely the claim the README's opening makes.

**The unchecked structural claims.** Read `partitions.csv` and confirm it still
holds only the factory app. Grep the generated `sdkconfig` for the WiFi and
Bluetooth symbols. Check whether a second image asset has appeared beside the
logo. These take a minute each and none of them is covered by CI.

**Fuzzer**, time-boxed. CI omits it deliberately — coverage-guided fuzzing has
no natural stopping point — so it only ever runs when someone chooses to run
it. The recipe and the last clean run are recorded in the header of
`tests/fuzz_parsers.c`. Report executions reached and any finding.

## Stage 4 — quality

Judge against the project's own stated conventions, in `CONTRIBUTING.md`, not
against generic taste: the `seedtool_`/`SEEDTOOL_` prefix on public names,
snake_case throughout, and comments that justify *why* rather than restating
*what*.

**Comment and documentation staleness is a first-class finding here.** Comments
in this codebase carry design reasoning — why a buffer is sized the way it is,
why a screen is ordered the way it is — so a comment that has outlived its code
is misinformation rather than clutter, and it is the failure mode this project
is most prone to, because the reasoning is dense and the code around it moves.
Cross-check comments against the code they describe, and README and
CONTRIBUTING against the commands and structures they document. A documented
build command that contradicts CI is the worked example of the genre.

Also confirm the dependency posture holds: `tools/make_logo.py` remains the one
place Pillow is used, and the firmware, verifier and tests stay dependency-free.

## Report

Order findings by severity. For each: what it is, `file:line`, what proves it
— or, when nothing does, what would.

End with the measured numbers in a fixed shape, so two audits can be compared
at a glance:

```
firmware      N bytes (P% of ceiling, R bytes free), delta vs <ref>
sections      .bss N, .data N, IRAM N
gates         self-test / tests N passed / QR smoke / sanitized / ELF audit
coverage      N static asserts, N source-grep pins, N self-test checks
unenforced    N stated invariants with nothing checking them
```

Say plainly when a stage was skipped and why. An audit that quietly omits the
firmware build because the toolchain drifted is worse than one that says so.
