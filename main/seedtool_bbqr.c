#include "seedtool_bbqr.h"

#include <string.h>

static const char BASE32_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

size_t seedtool_bbqr_base32_len(const size_t len) { return (len * 8 + 4) / 5; }

bool seedtool_bbqr_base32_encode(const uint8_t* data, const size_t len, char* out, const size_t out_len)
{
    const size_t needed = seedtool_bbqr_base32_len(len);
    if (out_len < needed) {
        return false;
    }
    uint32_t buffer = 0;
    unsigned bits = 0;
    size_t written = 0;
    for (size_t i = 0; i < len; ++i) {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[written++] = BASE32_ALPHABET[(buffer >> bits) & 0x1f];
        }
    }
    if (bits > 0) {
        out[written++] = BASE32_ALPHABET[(buffer << (5 - bits)) & 0x1f];
    }
    return written == needed;
}

/* Even split of `len` bytes across `part_count` parts, rounded up to a whole
 * 5-byte (8 base32 character) group: BBQr readers pad and base32-decode each
 * part on its own before concatenating raw bytes, so only a part boundary
 * that falls on a 5-byte group leaves every part but the last independently
 * decodable. Krux's own chunker (src/krux/qr.py, find_min_num_parts) does the
 * same rounding on the already-encoded base32 string; this does it one layer
 * earlier, on the raw bytes, since seedtool_bbqr_part encodes each part's
 * slice separately rather than encoding once and then slicing the string. */
static size_t part_size_bytes(const size_t len, const size_t part_count)
{
    if (!part_count) {
        return 0;
    }
    const size_t even = len / part_count;
    return ((even + 4) / 5) * 5;
}

size_t seedtool_bbqr_part_count(const size_t len, const size_t frame_chars)
{
    if (frame_chars <= SEEDTOOL_BBQR_HEADER_LEN) {
        return 0;
    }
    const size_t max_part_bytes = ((frame_chars - SEEDTOOL_BBQR_HEADER_LEN) / 8) * 5;
    if (!max_part_bytes) {
        return 0;
    }
    if (!len) {
        return 1;
    }
    return (len + max_part_bytes - 1) / max_part_bytes;
}

static bool int2base36(const size_t n, char out[2])
{
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (n > 1295) { /* largest two-digit base36 value: 35*36 + 35 */
        return false;
    }
    out[0] = digits[n / 36];
    out[1] = digits[n % 36];
    return true;
}

bool seedtool_bbqr_part(const uint8_t* data, const size_t len, const char encoding, const char file_type,
    const size_t part_index, const size_t part_count, char* out, const size_t out_len)
{
    char total_digits[2], index_digits[2];
    if (!part_count || part_index >= part_count || !int2base36(part_count, total_digits)
        || !int2base36(part_index, index_digits)) {
        return false;
    }
    const size_t size = part_size_bytes(len, part_count);
    const size_t offset = part_index * size;
    if (offset > len) {
        return false;
    }
    const size_t this_len = part_index + 1 == part_count ? len - offset : size;
    /* The one place a length was taken on trust rather than checked.
     * part_size_bytes rounds the average part up to a multiple of five, so a
     * `part_count` that did not come from seedtool_bbqr_part_count can leave
     * `size` too large for the tail: at len=7 and part_count=3 the size is 5,
     * and part 1 would read data[5..9] - three bytes past the buffer, encoded
     * straight into a QR on screen. Every caller does derive the count from
     * seedtool_bbqr_part_count, so this was never reachable, but the function
     * is exported and reads a caller's buffer, so it verifies its own
     * arithmetic rather than inheriting the guarantee. */
    if (this_len > len - offset) {
        return false;
    }
    const size_t payload_chars = seedtool_bbqr_base32_len(this_len);
    if (out_len < SEEDTOOL_BBQR_HEADER_LEN + payload_chars + 1) {
        return false;
    }
    out[0] = 'B';
    out[1] = '$';
    out[2] = encoding;
    out[3] = file_type;
    out[4] = total_digits[0];
    out[5] = total_digits[1];
    out[6] = index_digits[0];
    out[7] = index_digits[1];
    if (!seedtool_bbqr_base32_encode(
            data + offset, this_len, out + SEEDTOOL_BBQR_HEADER_LEN, out_len - SEEDTOOL_BBQR_HEADER_LEN)) {
        return false;
    }
    out[SEEDTOOL_BBQR_HEADER_LEN + payload_chars] = '\0';
    return true;
}
