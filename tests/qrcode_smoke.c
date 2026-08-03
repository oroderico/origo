#include "qrcode.h"

#include <assert.h>
#include <stdint.h>

int main(void)
{
    uint8_t modules[qrcode_getBufferSize(5)];
    QRCode qr;
    assert(qrcode_initText(&qr, modules, 4, ECC_LOW, "invalid version") == -1);
    assert(qrcode_initText(&qr, modules, 5, ECC_LOW,
               "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu")
        == 0);
    assert(qr.version == 5);
    assert(qr.size == 37);
    assert(qrcode_getModule(&qr, 0, 0));
    assert(!qrcode_getModule(&qr, 1, 1));
    assert(qrcode_getModule(&qr, 3, 3));
    assert(qrcode_getModule(&qr, 36, 0));
    assert(qrcode_getModule(&qr, 0, 36));
    return 0;
}
