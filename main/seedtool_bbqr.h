#ifndef SEEDTOOL_BBQR_H_
#define SEEDTOOL_BBQR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* BBQr (github.com/coinkite/BBQr) part header is fixed at 8 characters:
 * "B$" + encoding(1) + file_type(1) + total(2, base36) + index(2, base36). */
#define SEEDTOOL_BBQR_HEADER_LEN 8

/* How many characters seedtool_bbqr_base32_encode needs for `len` input
 * bytes: base32 packs 5 bits per output character, so ceil(len*8/5). */
size_t seedtool_bbqr_base32_len(size_t len);

/* RFC4648 base32 (alphabet A-Z2-7), upper case, no padding -- the encoding
 * BBQr's plain ("2") scheme uses. Writes exactly seedtool_bbqr_base32_len(len)
 * characters (no trailing NUL) into `out`, which must be at least that long.
 * Returns false if it is not. */
bool seedtool_bbqr_base32_encode(const uint8_t* data, size_t len, char* out, size_t out_len);

/* How many BBQr parts (QR frames) `len` bytes of payload needs, keeping every
 * part's header-plus-base32-payload within `frame_chars` alphanumeric-mode
 * characters (see seedtool_render_qr_alphanumeric_capacity) -- 0 if even one
 * part would not fit. Parts are split on whole 5-byte groups (8 base32
 * characters) wherever possible, except the last, so each part's base32
 * chunk is independently decodable the way BBQr readers expect (Krux's own
 * chunker, src/krux/qr.py find_min_num_parts, rounds the same way). */
size_t seedtool_bbqr_part_count(size_t len, size_t frame_chars);

/* Builds part `part_index` (0-based, < part_count) of `data`, `len` bytes
 * long, as a BBQr alphanumeric-mode QR payload string: "B$" + `encoding` +
 * `file_type` + base36(part_count) + base36(part_index) + a base32 chunk of
 * `data`, NUL-terminated. `part_count` must be what
 * seedtool_bbqr_part_count(len, frame_chars) returned for the same `len` and
 * the frame budget the caller sized `out` to; `part_count` above 1295 (the
 * largest two-digit base36 value) is out of BBQr's range. Returns false if
 * `out_len` is too small or the arguments are out of range. */
bool seedtool_bbqr_part(const uint8_t* data, size_t len, char encoding, char file_type, size_t part_index,
    size_t part_count, char* out, size_t out_len);

#endif
