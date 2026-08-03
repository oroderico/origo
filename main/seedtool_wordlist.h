#ifndef SEEDTOOL_WORDLIST_H_
#define SEEDTOOL_WORDLIST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_WORDLIST_LEN 2048
#define SEEDTOOL_MAX_WORD_LEN 8
#define SEEDTOOL_LETTERS 26

/* Below this many candidates the caller stops asking for letters and offers the
 * remaining words directly. */
#define SEEDTOOL_MAX_WORD_CHOICES 10

/* Total number of BIP39 English words starting with `prefix`. The first
 * `output_len` matching indices are written to `output`. */
size_t seedtool_words_with_prefix(const char* prefix, size_t prefix_len, uint16_t* output, size_t output_len);

/* For each letter a..z, whether appending it to `prefix` still leaves at least
 * one BIP39 word. Returns how many letters were enabled, which is never zero
 * for a prefix that itself has candidates. */
size_t seedtool_next_letters(const char* prefix, size_t prefix_len, bool enabled[SEEDTOOL_LETTERS]);

const char* seedtool_word(size_t index);

#endif
