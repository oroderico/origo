#include "seedtool_app.h"
#include "seedtool_core.h"
#include "seedtool_render.h"

#include <stdio.h>
#include <string.h>
#include <wally_core.h>

static int self_test(void)
{
    static const char mnemonic[]
        = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    static const char expected[] = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    if (wally_init(0) != WALLY_OK || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP84, 0, address, sizeof(address)) != SEEDTOOL_OK
        || strcmp(address, expected) != 0) {
        fputs("Origo host self-test failed\n", stderr);
        return 1;
    }
    seedtool_render_screen("ORIGO", "HOST SELF-TEST", address, "OK");
    if (!seedtool_render_qr(address)) {
        fputs("Origo QR self-test failed\n", stderr);
        return 1;
    }
    seedtool_zero(address, sizeof(address));
    wally_cleanup(0);
    puts("Origo host self-test OK");
    return 0;
}

int main(const int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return self_test();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
        return 2;
    }
    seedtool_run();
}
