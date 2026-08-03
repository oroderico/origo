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
