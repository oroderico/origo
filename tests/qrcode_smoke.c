/* Compiled the way the firmware compiles it: no -DLOCK_VERSION, so
 * qrcode.h's default of 0 applies and the encoder's own `version !=
 * LOCK_VERSION` guards are preprocessed out. This file used to be built with
 * -DLOCK_VERSION=5 and asserted that an over-large version is rejected, which
 * was true only of that build - the shipped encoder accepts any version it is
 * handed and has a "@TODO: Return error if data is too big" where the length
 * check would go. Testing the configuration that ships matters more than
 * testing the one that made the encoder look defensive.
 *
 * What actually keeps the firmware safe is the pre-check every caller in
 * seedtool_render.c performs: ask qrcode_versionFor{Text,Bytes} which version
 * holds the payload, refuse to draw when it answers 0, and allocate the
 * module buffer for QR_VERSION - the largest - rather than the version drawn.
 * Those are the properties asserted below. */
#include "qrcode.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define MAX_VERSION 6 /* QR_VERSION in main/seedtool_render.c */

int main(void)
{
    uint8_t modules[qrcode_getBufferSize(MAX_VERSION)];
    QRCode qr;

    /* The published byte capacities at ECC_LOW. The callers depend on these
     * being right: a version reported as holding more than it does is what
     * would let a payload overrun the encoder's own codeword buffer. */
    assert(qrcode_versionForBytes(ECC_LOW, 17, MAX_VERSION) == 1);
    assert(qrcode_versionForBytes(ECC_LOW, 18, MAX_VERSION) == 2);
    assert(qrcode_versionForBytes(ECC_LOW, 32, MAX_VERSION) == 2);
    assert(qrcode_versionForBytes(ECC_LOW, 33, MAX_VERSION) == 3);
    assert(qrcode_versionForBytes(ECC_LOW, 134, MAX_VERSION) == MAX_VERSION);

    /* Zero for anything past the largest version, which is the answer the
     * callers turn into "Too long for a QR" rather than drawing. */
    assert(qrcode_versionForBytes(ECC_LOW, 135, MAX_VERSION) == 0);
    assert(qrcode_versionForBytes(ECC_LOW, 1000, MAX_VERSION) == 0);

    /* A Compact SeedQR's raw entropy: 16 bytes for 12 words, 32 for 24, drawn
     * at the smallest version that holds each. */
    assert(qrcode_versionForBytes(ECC_LOW, 16, MAX_VERSION) == 1);
    assert(qrcode_versionForBytes(ECC_LOW, 32, MAX_VERSION) == 2);

    /* Text mode, the account-key and address path. */
    char oversized[400];
    memset(oversized, 'A', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    assert(qrcode_versionForText(ECC_LOW, oversized, MAX_VERSION) == 0);

    /* The buffer the callers allocate is sized for MAX_VERSION and reused for
     * whatever smaller version gets drawn, so getBufferSize must not shrink
     * as the version grows. */
    for (uint8_t v = 2; v <= MAX_VERSION; ++v) {
        assert(qrcode_getBufferSize(v) >= qrcode_getBufferSize((uint8_t)(v - 1)));
    }

    /* An address at the version the encoder itself picks for it. */
    const char* const address = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
    const uint8_t version = qrcode_versionForText(ECC_LOW, address, MAX_VERSION);
    assert(version != 0 && version <= MAX_VERSION);
    assert(qrcode_initText(&qr, modules, version, ECC_LOW, address) == 0);
    assert(qr.version == version);
    assert(qr.size == 17 + 4 * version);
    /* The three finder patterns: dark at each corner they occupy. */
    assert(qrcode_getModule(&qr, 0, 0));
    assert(!qrcode_getModule(&qr, 1, 1));
    assert(qrcode_getModule(&qr, 3, 3));
    assert(qrcode_getModule(&qr, (uint8_t)(qr.size - 1), 0));
    assert(qrcode_getModule(&qr, 0, (uint8_t)(qr.size - 1)));

    /* Byte mode at the largest version, the widest thing ever drawn. */
    uint8_t payload[134];
    memset(payload, 0xa5, sizeof(payload));
    assert(qrcode_versionForBytes(ECC_LOW, sizeof(payload), MAX_VERSION) == MAX_VERSION);
    assert(qrcode_initBytes(&qr, modules, MAX_VERSION, ECC_LOW, payload, sizeof(payload)) == 0);
    assert(qr.size == 17 + 4 * MAX_VERSION);
    return 0;
}
