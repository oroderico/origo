#include "seedtool_wordlist.h"

#include <string.h>

#include <wally_bip39.h>

const char* seedtool_word(const size_t index)
{
    return index < SEEDTOOL_WORDLIST_LEN ? bip39_get_word_by_index(NULL, index) : NULL;
}

/* The BIP39 English wordlist is sorted, so the words matching the first
 * `prefix_len` characters of `prefix` are one run: [*first, returned end). */
static size_t prefix_run(const char* prefix, const size_t prefix_len, size_t* first)
{
    size_t low = 0, high = SEEDTOOL_WORDLIST_LEN;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (strncmp(seedtool_word(mid), prefix, prefix_len) < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    size_t end = low;
    while (end < SEEDTOOL_WORDLIST_LEN && strncmp(seedtool_word(end), prefix, prefix_len) == 0) {
        ++end;
    }
    *first = low;
    return end;
}

int seedtool_word_index(const char* word)
{
    if (!word) {
        return -1;
    }
    size_t first = 0;
    /* Comparing the terminator too makes the run an exact match. */
    return prefix_run(word, strlen(word) + 1, &first) > first ? (int)first : -1;
}

void seedtool_wordset_clear(seedtool_wordset_t* set)
{
    if (set) {
        memset(set->bits, 0, sizeof(set->bits));
    }
}

void seedtool_wordset_add(seedtool_wordset_t* set, const size_t index)
{
    if (set && index < SEEDTOOL_WORDLIST_LEN) {
        set->bits[index / 8] |= (uint8_t)(1u << (index % 8));
    }
}

/* A NULL set is the whole wordlist, so every caller that has nothing to narrow
 * by can pass nothing rather than building a set of all 2048. */
bool seedtool_wordset_has(const seedtool_wordset_t* set, const size_t index)
{
    if (index >= SEEDTOOL_WORDLIST_LEN) {
        return false;
    }
    return !set || (set->bits[index / 8] & (uint8_t)(1u << (index % 8))) != 0;
}

size_t seedtool_words_with_prefix_in(const seedtool_wordset_t* allowed, const char* prefix, const size_t prefix_len,
    uint16_t* output, const size_t output_len)
{
    if (!prefix || !output || !output_len) {
        return 0;
    }
    size_t matches = 0, first = 0;
    const size_t end = prefix_run(prefix, prefix_len, &first);
    for (size_t index = first; index < end; ++index) {
        if (!seedtool_wordset_has(allowed, index)) {
            continue;
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

unsigned seedtool_word_number_in(const seedtool_wordset_t* allowed, const char* digits, const size_t digits_len)
{
    unsigned value = 0;
    if (!digits || !digits_len || digits_len > SEEDTOOL_MAX_WORD_DIGITS || !digits_value(digits, digits_len, &value)) {
        return 0;
    }
    if (value < 1 || value > SEEDTOOL_WORDLIST_LEN) {
        return 0;
    }
    /* One-based on the way in, zero-based in the set. */
    return seedtool_wordset_has(allowed, value - 1) ? value : 0;
}

/* Whether any word number in [low, high] is in `allowed`, with the range
 * clamped to the wordlist first. The ranges below can run well past 2048, so
 * this scans the overlap rather than the range as given. */
static bool wordset_any_in_range(const seedtool_wordset_t* allowed, const unsigned low, const unsigned high)
{
    const unsigned first = low < 1 ? 1 : low;
    const unsigned last = high > SEEDTOOL_WORDLIST_LEN ? SEEDTOOL_WORDLIST_LEN : high;
    if (first > last) {
        return false;
    }
    if (!allowed) {
        return true;
    }
    for (unsigned number = first; number <= last; ++number) {
        if (seedtool_wordset_has(allowed, number - 1)) {
            return true;
        }
    }
    return false;
}

size_t seedtool_next_digits_in(
    const seedtool_wordset_t* allowed, const char* digits, const size_t digits_len, bool enabled[SEEDTOOL_DIGITS])
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
         * digit is offered only if one of them holds a word number the caller
         * still allows, which is what keeps 2049 unreachable while 2048 stays
         * typeable - and, with a set, what darkens every digit that could only
         * ever spell a word the mnemonic cannot end with. */
        unsigned low = prefix * 10 + digit, span = 1;
        for (size_t k = 0; k <= remaining; ++k) {
            if (wordset_any_in_range(allowed, low, low + span - 1)) {
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

size_t seedtool_next_letters_in(
    const seedtool_wordset_t* allowed, const char* prefix, const size_t prefix_len, bool enabled[SEEDTOOL_LETTERS])
{
    if (!prefix || !enabled) {
        return 0;
    }
    memset(enabled, 0, sizeof(bool) * SEEDTOOL_LETTERS);
    size_t count = 0, first = 0;
    const size_t end = prefix_run(prefix, prefix_len, &first);
    for (size_t index = first; index < end; ++index) {
        if (!seedtool_wordset_has(allowed, index)) {
            continue;
        }
        /* A word ending at the prefix has no next letter. */
        const char* const word = seedtool_word(index);
        if (strlen(word) <= prefix_len) {
            continue;
        }
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
