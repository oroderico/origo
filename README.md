# Origo

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

## Entropy transcripts

The transcript is shown before its full SHA256 and mnemonic. Record it so the
calculation can be reproduced independently.

| Source | 12 words | 24 words | Canonical transcript |
|---|---:|---:|---|
| D6 | 50 rolls | 99 rolls | digits concatenated, e.g. `123456` |
| D20 | 30 rolls | 60 rolls | decimal rolls joined by `-`, e.g. `1-20-7` |
| Coins | 128 flips | 256 flips | Heads=`1`, Tails=`0`, concatenated |
| Cards | first 25 distinct cards | unsupported | `cards-v1:` plus rank/suit codes |

Card ranks are `A23456789TJQK`; suits are `CDHS`. The canonical deck order is
`AC..KC`, `AD..KD`, `AH..KH`, `AS..KS`. Only the order of the first 25 distinct
cards is used. SHA256 is applied to the ASCII transcript. A 12-word seed uses
the first 16 hash bytes; a 24-word seed uses all 32 bytes.

Checksum completion consumes 11 BIP39 words plus exactly 7 coin flips, or 23
words plus exactly 3 flips. Those flips are the missing entropy bits and lead
to one checksum-valid final word; the firmware never randomly selects from the
128 or 8 otherwise-valid endings.

## Controls

The board has two buttons and no select button. The left button moves to the
previous item, the right button to the next, and both pressed together select.
Holding one button repeats it. A press is acted on only once both buttons are
released, so a chord is never mistaken for a step.

Words are typed on a letter keyboard. Only letters that still lead to a BIP39
word are reachable, and once ten or fewer words remain they are offered directly
instead. Deleting past the start of a word steps back to the previous one. The
suggestion order and the initially selected key are a pure function of what has
been typed. Neither is randomised: no screen in the entry path may depend on the
device RNG.

An optional printable-ASCII passphrase of at most 100 characters is typed on a
four-page keyboard covering the whole printable range, entered twice, and exists
for that derivation session only.

## Restore and inspect

Existing mnemonics are validated before derivation; a bad checksum blocks the
address viewer entirely. For a valid mnemonic the viewer shows:

- the master fingerprint;
- the watch-only account key at `m/84'/0'/0'` and `m/86'/0'/0'`, in standard
  BIP32 `xpub` serialisation, titled with its key origin such as
  `[73c5da0a/84'/0'/0']`. No SLIP-132 `zpub` variants are produced;
- mainnet receive addresses for indices 0 through 99 at `m/84'/0'/0'/0/i` and
  `m/86'/0'/0'/0/i`.

Long values are paged two lines at a time, split by what actually fits the
display rather than by a character count, so a proportional font can never drop
a character from a value that is about to be transcribed.

QR codes contain only the raw address. Mnemonics, passphrases, and xpubs are
never encoded as QR: an account xpub in a photographed QR would expose every
past and future address of that account.

## Run on a PC

The SDL simulator runs the firmware's actual application flow, deterministic
core, renderer, fonts, QR encoder and pinned libwally source. Only the display,
buttons, clock and secp256k1 blinding randomness are replaced by host adapters.

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
it verifies the published BIP84/BIP86 address and account xpub vectors, that
every prefix of every BIP39 word is reachable through the keyboard, and that
paged text reassembles byte for byte.

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
driver, two fonts and a version-5 QR encoder. Its libwally component is built
without Elements. The audit rejects linked wallet, radio, persistence, OTA update,
battery, generic graphics, transaction, PSBT and Elements symbols and enforces
a 285 KiB image limit. The partition table contains only the factory
application: there is no NVS, PHY-data or OTA slot.

## Independent verification

`tools/origo_verify.py` is dependency-free and independently implements
BIP39, BIP32, BIP84, BIP86 and Bech32/Bech32m. Examples:

```sh
python3 tools/origo_verify.py generate d20 --words 12 1 2 3 ...
python3 tools/origo_verify.py complete "abandon ... abandon" 0000000
python3 tools/origo_verify.py inspect "abandon ... about" --index 0
```

`inspect` prints the checksum verdict, master fingerprint, both account xpubs
and both addresses, so every value the device can display has an independent
second implementation to be compared against.

Do not type a real mnemonic or passphrase into a network-connected computer.
Boot a trusted offline environment, verify this repository and tool first, and
compare the transcript, full hash, fingerprint, xpubs, and addresses.

## Safety boundaries

This tool protects against a compromised hardware RNG only when the human
entropy source and recording process are sound. It cannot protect against a
compromised display, malicious firmware, biased physical dice/cards/coins,
shoulder surfing, or mistakes copying entropy. Reproduce the result on a second
independent implementation before funding an address. The ten-minute inactivity
timer gives a 60-second extend-or-erase warning; cancel, error, timeout, and
restart paths wipe session buffers before returning or rebooting.
