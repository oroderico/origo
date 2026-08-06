/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Richard Moore
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 *  Special thanks to Nayuki (https://www.nayuki.io/) from which this library was
 *  heavily inspired and compared against.
 *
 *  See: https://github.com/nayuki/QR-Code-generator/tree/master/cpp
 */

#ifndef __QRCODE_H_
#define __QRCODE_H_

/*#ifndef __cplusplus
   typedef unsigned char bool;
   static const bool false = 0;
   static const bool true = 1;
 #endif */

#include <stdbool.h>
#include <stdint.h>

// QR Code Format Encoding
#define MODE_NUMERIC 0
#define MODE_ALPHANUMERIC 1
#define MODE_BYTE 2

// Error Correction Code Levels
#define ECC_LOW 0
#define ECC_MEDIUM 1
#define ECC_QUARTILE 2
#define ECC_HIGH 3

// If set to non-zero, this library can ONLY produce QR codes at that version
// This saves a lot of dynamic memory, as the codeword tables are skipped
#ifndef LOCK_VERSION
#define LOCK_VERSION 0
#endif

typedef struct QRCode {
    uint8_t version;
    uint8_t size;
    uint8_t ecc;
    uint8_t mode;
    uint8_t mask;
    uint8_t* modules;
} QRCode;

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

uint16_t qrcode_getBufferSize(uint8_t version);

// Smallest version in 1..maxVersion whose byte-mode capacity at `ecc` fits
// `length` bytes, or 0 if none does.
uint8_t qrcode_versionForBytes(uint8_t ecc, uint16_t length, uint8_t maxVersion);

// Smallest version in 1..maxVersion whose alphanumeric-mode capacity at `ecc`
// fits `length` characters, or 0 if none does. `length` characters must all be
// in the alphanumeric mode charset (see getAlphanumeric) for a QR actually
// initialised at the returned version with qrcode_initText to land in
// alphanumeric mode rather than fall back to byte mode.
uint8_t qrcode_versionForAlphanumeric(uint8_t ecc, uint16_t length, uint8_t maxVersion);

// Smallest version in 1..maxVersion that actually holds `text` once
// qrcode_initText encodes it, whichever mode (numeric, alphanumeric or byte)
// that ends up being -- unlike qrcode_versionForBytes/ForAlphanumeric, which
// assume one mode up front, this checks `text`'s own charset first so the
// version returned is always safe to pass to qrcode_initText. Returns 0 if
// nothing in range fits. IMPORTANT: qrcode_initText/initBytes do not
// themselves check that `text`/`data` fits the `version` passed in -- calling
// them with a version too small for the content overruns their internal
// codeword buffer. Always size the version with one of these three functions
// first; never guess or shrink a version and try it directly.
uint8_t qrcode_versionForText(uint8_t ecc, const char* text, uint8_t maxVersion);

int8_t qrcode_initText(QRCode* qrcode, uint8_t* modules, uint8_t version, uint8_t ecc, const char* data);
int8_t qrcode_initBytes(QRCode* qrcode, uint8_t* modules, uint8_t version, uint8_t ecc, uint8_t* data, uint16_t length);

bool qrcode_getModule(QRCode* qrcode, uint8_t x, uint8_t y);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __QRCODE_H_ */
