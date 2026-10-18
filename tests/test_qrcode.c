/* The vendored QR encoder, compiled as the firmware compiles it: without
 * LOCK_VERSION, so it accepts any version it is handed and does not check the
 * payload fits. What keeps the firmware safe is the pre-check every caller in
 * seedtool_render.c makes: ask qrcode_versionFor{Text,Bytes} which version
 * holds the payload, refuse to draw on 0, and size the module buffer for the
 * largest version. Those are the properties checked here. */
#include "check.h"

#include "qrcode.h"

#include <stdint.h>
#include <string.h>

#define MAX_VERSION 6 /* QR_VERSION in main/seedtool_render.c */

static bool encoder_matches_what_callers_assume(void)
{
    uint8_t modules[qrcode_getBufferSize(MAX_VERSION)];
    QRCode qr;

    /* The published byte capacities at ECC_LOW. The callers depend on these
     * being right: a version reported as holding more than it does is what
     * would let a payload overrun the encoder's own codeword buffer. */
    CHECK(qrcode_versionForBytes(ECC_LOW, 17, MAX_VERSION) == 1);
    CHECK(qrcode_versionForBytes(ECC_LOW, 18, MAX_VERSION) == 2);
    CHECK(qrcode_versionForBytes(ECC_LOW, 32, MAX_VERSION) == 2);
    CHECK(qrcode_versionForBytes(ECC_LOW, 33, MAX_VERSION) == 3);
    CHECK(qrcode_versionForBytes(ECC_LOW, 134, MAX_VERSION) == MAX_VERSION);

    /* Zero for anything past the largest version, which is the answer the
     * callers turn into "Too long for a QR" rather than drawing. */
    CHECK(qrcode_versionForBytes(ECC_LOW, 135, MAX_VERSION) == 0);
    CHECK(qrcode_versionForBytes(ECC_LOW, 1000, MAX_VERSION) == 0);

    /* A Compact SeedQR's raw entropy: 16 bytes for 12 words, 32 for 24, drawn
     * at the smallest version that holds each. */
    CHECK(qrcode_versionForBytes(ECC_LOW, 16, MAX_VERSION) == 1);
    CHECK(qrcode_versionForBytes(ECC_LOW, 32, MAX_VERSION) == 2);

    /* Text mode, the account-key and address path. */
    char oversized[400];
    memset(oversized, 'A', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    CHECK(qrcode_versionForText(ECC_LOW, oversized, MAX_VERSION) == 0);

    /* The buffer the callers allocate is sized for MAX_VERSION and reused for
     * whatever smaller version gets drawn, so getBufferSize must not shrink
     * as the version grows. */
    for (uint8_t v = 2; v <= MAX_VERSION; ++v) {
        CHECK(qrcode_getBufferSize(v) >= qrcode_getBufferSize((uint8_t)(v - 1)));
    }

    /* An address at the version the encoder itself picks for it. */
    const char* const address = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
    const uint8_t version = qrcode_versionForText(ECC_LOW, address, MAX_VERSION);
    CHECK(version != 0 && version <= MAX_VERSION);
    CHECK(qrcode_initText(&qr, modules, version, ECC_LOW, address) == 0);
    CHECK(qr.version == version);
    CHECK(qr.size == 17 + 4 * version);
    /* The three finder patterns: dark at each corner they occupy. */
    CHECK(qrcode_getModule(&qr, 0, 0));
    CHECK(!qrcode_getModule(&qr, 1, 1));
    CHECK(qrcode_getModule(&qr, 3, 3));
    CHECK(qrcode_getModule(&qr, (uint8_t)(qr.size - 1), 0));
    CHECK(qrcode_getModule(&qr, 0, (uint8_t)(qr.size - 1)));

    /* Byte mode at the largest version, the widest thing ever drawn. */
    uint8_t payload[134];
    memset(payload, 0xa5, sizeof(payload));
    CHECK(qrcode_versionForBytes(ECC_LOW, sizeof(payload), MAX_VERSION) == MAX_VERSION);
    CHECK(qrcode_initBytes(&qr, modules, MAX_VERSION, ECC_LOW, payload, sizeof(payload)) == 0);
    CHECK(qr.size == 17 + 4 * MAX_VERSION);
    return true;
}

int main(void)
{
    static const test_t tests[] = { TEST(encoder_matches_what_callers_assume) };
    return run_tests("qrcode", tests, sizeof(tests) / sizeof(tests[0]));
}
