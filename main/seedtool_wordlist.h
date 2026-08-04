#ifndef SEEDTOOL_WORDLIST_H_
#define SEEDTOOL_WORDLIST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_WORDLIST_LEN 2048
#define SEEDTOOL_MAX_WORD_LEN 8
#define SEEDTOOL_LETTERS 26
#define SEEDTOOL_DIGITS 10

/* 2048 is four digits, and a printed wordlist pads to four. */
#define SEEDTOOL_MAX_WORD_DIGITS 4

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

/* The value of `digits` as a one-based BIP39 word number, or 0 when it is not
 * one. One-based is the position in a printed wordlist: `abandon` is 1 and `zoo`
 * is 2048, one more than the zero-based index the encoding itself uses. Leading
 * zeros are accepted, because a printed list pads to four digits and whoever
 * reads `0004` off it types all four. */
unsigned seedtool_word_number(const char* digits, size_t digits_len);

/* For each digit 0..9, whether appending it to `digits` still leaves a word
 * number reachable within SEEDTOOL_MAX_WORD_DIGITS characters. Returns how many
 * were enabled, which is never zero while a word number is still reachable. */
size_t seedtool_next_digits(const char* digits, size_t digits_len, bool enabled[SEEDTOOL_DIGITS]);

const char* seedtool_word(size_t index);

#endif
