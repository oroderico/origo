# Safety boundaries

What Origo protects against, and what it does not.

This tool protects against a compromised hardware RNG only when the human
entropy source and recording process are sound. It cannot protect against a
compromised display, malicious firmware, biased physical dice/coins/tickets,
shoulder surfing, mistakes copying entropy, or a Compact SeedQR photographed
by anyone other than the person who asked to see it. Reproduce the result on a
second independent implementation before funding an address.
`Find by ending` can say an address is this wallet's; it cannot say an address
that is not found belongs to no one, since it searches only the type, account
and passphrase now set.

Nor does it defend against someone reading the board's memory directly. There
is no secure element, no secure boot, no flash encryption, and JTAG is not
fused off — this is a development board running an offline calculator, not a
hardware wallet, and it is not built to survive an attacker who has the device
in hand. What that leaves is a housekeeping obligation rather than a defence:
every screen that holds seed material wipes its buffers when it is left, so
what is in RAM is what the screen in front of you needs and not what three
screens ago needed. Power the board down when you are done rather than leaving
a session open.

The ten-minute inactivity timer gives a 60-second warning; any button extends
the session, and a warning left unanswered restarts the device, from any
screen. So does leaving the wallet. Erasing on purpose is the wallet's own
`Erase and restart` row: a timer meant for a reader who walked away has no
business turning a stray press into the loss of a half-entered run. A software restart leaves RAM as it was, so every boot zeroes the free
heap, where the previous session's stacks lived, and C startup zeroes the
rest. That covers an internal assertion failure too (`__wrap_abort` in
`main/seedtool_platform_esp.c`), which restarts without unwinding.
