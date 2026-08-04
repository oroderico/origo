#include "seedtool_wordlist.h"

#include <string.h>

#include <wally_bip39.h>

/* The BIP39 English wordlist is sorted, so a prefix scan can stop as soon as it
 * walks past the matching run. No RNG is involved anywhere here: the candidate
 * order and the enabled letters are a pure function of what the user typed. */

const char* seedtool_word(const size_t index)
{
    return index < SEEDTOOL_WORDLIST_LEN ? bip39_get_word_by_index(NULL, index) : NULL;
}

size_t seedtool_words_with_prefix(
    const char* prefix, const size_t prefix_len, uint16_t* output, const size_t output_len)
{
    if (!prefix || !output || !output_len) {
        return 0;
    }
    size_t matches = 0;
    for (size_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        const char* const word = seedtool_word(index);
        const int order = strncmp(word, prefix, prefix_len);
        if (order < 0) {
            continue;
        }
        if (order > 0) {
            break;
        }
        if (matches < output_len) {
            output[matches] = (uint16_t)index;
        }
        ++matches;
    }
    return matches;
}

/* The digits read as a plain integer, whether or not it is a word number. At
 * most four digits, so 9999 is the largest value this can produce. */
static bool digits_value(const char* digits, const size_t digits_len, unsigned* value)
{
    unsigned result = 0;
    for (size_t i = 0; i < digits_len; ++i) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
        result = result * 10 + (unsigned)(digits[i] - '0');
    }
    *value = result;
    return true;
}

unsigned seedtool_word_number(const char* digits, const size_t digits_len)
{
    unsigned value = 0;
    if (!digits || !digits_len || digits_len > SEEDTOOL_MAX_WORD_DIGITS || !digits_value(digits, digits_len, &value)) {
        return 0;
    }
    return value >= 1 && value <= SEEDTOOL_WORDLIST_LEN ? value : 0;
}

size_t seedtool_next_digits(const char* digits, const size_t digits_len, bool enabled[SEEDTOOL_DIGITS])
{
    if (!digits || !enabled) {
        return 0;
    }
    memset(enabled, 0, sizeof(bool) * SEEDTOOL_DIGITS);
    unsigned prefix = 0;
    if (digits_len >= SEEDTOOL_MAX_WORD_DIGITS || !digits_value(digits, digits_len, &prefix)) {
        return 0;
    }
    const size_t remaining = SEEDTOOL_MAX_WORD_DIGITS - digits_len - 1;
    size_t count = 0;
    for (unsigned digit = 0; digit < SEEDTOOL_DIGITS; ++digit) {
        /* With `remaining` digits still to come, what this prefix can still
         * reach are the ranges [low, low + span - 1] as it is padded out. The
         * digit is offered only if one of them holds a word number, which is
         * what keeps 2049 unreachable while 2048 stays typeable. */
        unsigned low = prefix * 10 + digit, span = 1;
        for (size_t k = 0; k <= remaining; ++k) {
            if (low + span - 1 >= 1 && low <= SEEDTOOL_WORDLIST_LEN) {
                enabled[digit] = true;
                ++count;
                break;
            }
            low *= 10;
            span *= 10;
        }
    }
    return count;
}

size_t seedtool_next_letters(const char* prefix, const size_t prefix_len, bool enabled[SEEDTOOL_LETTERS])
{
    if (!prefix || !enabled) {
        return 0;
    }
    memset(enabled, 0, sizeof(bool) * SEEDTOOL_LETTERS);
    size_t count = 0;
    for (size_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        const char* const word = seedtool_word(index);
        const int order = strncmp(word, prefix, prefix_len);
        if (order < 0) {
            continue;
        }
        if (order > 0) {
            break;
        }
        /* The word may end exactly at the prefix; there is no next letter then. */
        const char next = word[prefix_len];
        if (next < 'a' || next > 'z') {
            continue;
        }
        if (!enabled[next - 'a']) {
            enabled[next - 'a'] = true;
            ++count;
        }
    }
    return count;
}
