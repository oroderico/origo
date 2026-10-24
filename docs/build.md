# Build, flash and simulate

Building the firmware reproducibly, flashing it, and running the same app on a PC.

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
a 512 KiB image ceiling — half the 1 MiB factory partition, so a budget for
keeping the firmware readable rather than a limit of the hardware. LEDC is the
one peripheral driver let through that list, solely for the backlight's
brightness PWM - it carries no wallet, radio or persistence surface of its own. The partition table contains only the factory
application: there is no NVS, PHY-data or OTA slot.

Wi-Fi and Bluetooth are absent rather than disabled. Neither appears in
`main/CMakeLists.txt`'s component list, so neither is ever pulled into the
build, and the options that would switch them off — `CONFIG_ESP_WIFI_ENABLED`,
`CONFIG_BT_ENABLED` — are not present in the generated `sdkconfig` at all,
because the components that declare them are not there to declare anything.
That is a stronger statement than a flag set to `n`, and deliberately so: a
flag is a configuration that can drift, which is the exact shape of the
regression described in [Why Origo](why.md#why). The audit checks the result from both
ends, rejecting `esp_wifi_init`, `esp_wifi_start` and `nimble_port_init` among
the linked symbols and `libesp_wifi.a` and `libbt.a` among the archive members
the map file shows were actually linked.

The build is reproducible: the same source and the same ESP-IDF produce a
byte-identical binary, which is what makes a published SHA256 worth anything.
Check it the way CI does, by building twice into separate directories and
comparing:

```sh
idf.py -B "$PWD/build-a" -D SDKCONFIG="$PWD/build-a/sdkconfig" build
idf.py -B "$PWD/build-b" -D SDKCONFIG="$PWD/build-b/sdkconfig" build
cmp build-a/origo.bin build-b/origo.bin
```

A difference here means something outside the source — a path, a timestamp, a
toolchain version — reached the image, and the published hash stops being a
statement anyone else can confirm. `CONFIG_APP_REPRODUCIBLE_BUILD=y` and
`CONFIG_APP_EXCLUDE_PROJECT_NAME_VAR=y` in `sdkconfig.defaults` are what keep
build paths and project metadata out of it.

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
as you would on the device. `Q` or `Escape` closes the simulator.

The tests need no SDL: `cmake -S host -B build-host && cmake --build build-host
&& ctest --test-dir build-host`. `tests/test_core.c` checks the published BIP39,
BIP44, BIP49, BIP84, BIP86, SLIP-132 and BIP380 vectors, exhaustive wordlist
properties, and that every dice and ticket word is reached equally often;
`tests/test_render.c` checks that what the screens draw is the value held (paged
text, grouped addresses, Stackbit grids, Compact SeedQR payloads) and that the
keyboards reach every character.

The simulator is for development with published test vectors. Do not enter a
real mnemonic, passphrase or rolls on a network-connected PC.
