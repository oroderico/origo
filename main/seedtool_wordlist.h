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

/* A subset of the wordlist, one bit per zero-based index. Entry screens narrow
 * to a subset when the words still possible are fewer than all of them: the
 * final word of a mnemonic is the case that matters, where the checksum leaves
 * only 128 of the 2048 possible for 12 words, and 8 for 24. 256 bytes, which
 * sits on a stack frame like every other buffer here.
 *
 * A NULL set means the whole wordlist everywhere one is accepted, so a caller
 * with nothing to narrow by passes nothing. */
#define SEEDTOOL_WORDSET_BYTES (SEEDTOOL_WORDLIST_LEN / 8)

typedef struct {
    uint8_t bits[SEEDTOOL_WORDSET_BYTES];
} seedtool_wordset_t;

void seedtool_wordset_clear(seedtool_wordset_t* set);
void seedtool_wordset_add(seedtool_wordset_t* set, size_t index);
bool seedtool_wordset_has(const seedtool_wordset_t* set, size_t index);
/* How many words the set holds. */
size_t seedtool_wordset_count(const seedtool_wordset_t* set);

/* Every function below that takes `allowed` counts only words in it, NULL
 * meaning the whole wordlist. */

/* Number of words starting with `prefix`. The first `output_len` matching
 * indices are written to `output`. */
size_t seedtool_words_with_prefix_in(
    const seedtool_wordset_t* allowed, const char* prefix, size_t prefix_len, uint16_t* output, size_t output_len);

/* For each letter a..z, whether appending it to `prefix` still leaves at least
 * one word. Returns how many letters were enabled, which is never zero for a
 * prefix that itself has candidates. */
size_t seedtool_next_letters_in(
    const seedtool_wordset_t* allowed, const char* prefix, size_t prefix_len, bool enabled[SEEDTOOL_LETTERS]);

/* The value of `digits` as a one-based word number (`abandon` = 1, `zoo` =
 * 2048, as printed), or 0 when it is not one. Leading zeros are accepted: a
 * printed list pads to four digits. */
unsigned seedtool_word_number_in(const seedtool_wordset_t* allowed, const char* digits, size_t digits_len);

/* For each digit 0..9, whether appending it to `digits` still leaves a word
 * number reachable within SEEDTOOL_MAX_WORD_DIGITS characters. Returns how many
 * were enabled, which is never zero while a word number is still reachable. */
size_t seedtool_next_digits_in(
    const seedtool_wordset_t* allowed, const char* digits, size_t digits_len, bool enabled[SEEDTOOL_DIGITS]);

const char* seedtool_word(size_t index);

#endif
