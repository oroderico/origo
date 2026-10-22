/* Core, wordlist and BBQr: published vectors and exhaustive properties. */
#include "check.h"
#include "vectors.h"

#include "seedtool_bbqr.h"
#include "seedtool_core.h"
#include "seedtool_wordlist.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

/* Eleven flips name one word, most significant bit first, and that index must
 * survive seedtool_complete_checksum into the finished mnemonic, for all 2048
 * words. The all-zero case is pinned to the published vectors: eleven (or
 * twenty-three) index-0 words plus zero bits are `mnemonic` (or `mnemonic24`). */
static bool coin_words_build_the_entropy(const size_t words, const uint16_t index, char* out, const size_t out_len)
{
    const size_t word_count = words == 12 ? 11 : 23;
    const size_t tail_count = words == 12 ? 7 : 3;
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    uint8_t tail[7] = { 0 };
    size_t used = 0;
    for (size_t i = 0; i < word_count; ++i) {
        const char* const word = seedtool_word(index);
        const size_t len = strlen(word);
        if (used + len + (i ? 1 : 0) >= sizeof(prefix)) {
            return false;
        }
        if (i) {
            prefix[used++] = ' ';
        }
        memcpy(prefix + used, word, len);
        used += len;
        prefix[used] = '\0';
    }
    return seedtool_complete_checksum(prefix, tail, tail_count, out, out_len) == SEEDTOOL_OK;
}

static bool coin_word_encoding_is_sound(void)
{
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1];

    /* Zero flips, both lengths, against the published vectors. */
    if (!coin_words_build_the_entropy(12, 0, completed, sizeof(completed)) || strcmp(completed, mnemonic) != 0) {
        return false;
    }
    if (!coin_words_build_the_entropy(24, 0, completed, sizeof(completed)) || strcmp(completed, mnemonic24) != 0) {
        return false;
    }

    /* Every word survives the trip, at both lengths: whatever the reader was
     * shown on the confirmation screen is what the finished mnemonic spells. */
    for (uint16_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        const size_t lengths[] = { 12, 24 };
        for (size_t l = 0; l < 2; ++l) {
            const size_t word_count = lengths[l] == 12 ? 11 : 23;
            uint16_t numbers[24];
            size_t count = 0;
            if (!coin_words_build_the_entropy(lengths[l], index, completed, sizeof(completed))
                || seedtool_mnemonic_word_numbers(completed, numbers, 24, &count) != SEEDTOOL_OK
                || count != lengths[l]) {
                return false;
            }
            for (size_t i = 0; i < word_count; ++i) {
                if (numbers[i] != index + 1) {
                    return false;
                }
            }
        }
    }

    /* The top index must be reachable without overflowing into a twelfth bit. */
    return SEEDTOOL_WORDLIST_LEN == (1u << 11);
}

/* SeedPicker's tickets: every word is named by exactly one ticket and die, and
 * the only pairs refused are the four past 2048 and those out of range. The
 * worked example in docs/entropy.md finishes to SeedPicker's own 24th word when
 * the loose bits are three tails, as its calculator picks. */
static bool ticket_words_cover_the_wordlist(void)
{
    unsigned hits[SEEDTOOL_WORDLIST_LEN] = { 0 };
    unsigned refused = 0;
    for (unsigned ticket = 0; ticket <= SEEDTOOL_TICKETS + 1; ++ticket) {
        for (unsigned die = 0; die <= SEEDTOOL_TICKET_DIE + 1; ++die) {
            uint16_t index = 0;
            const bool in_range = ticket >= 1 && ticket <= SEEDTOOL_TICKETS && die >= 1 && die <= SEEDTOOL_TICKET_DIE;
            if (seedtool_ticket_word(ticket, die, &index) != SEEDTOOL_OK) {
                /* In range, only T342 with 3 to 6 may be refused. */
                if (in_range && !(ticket == SEEDTOOL_TICKETS && die >= 3)) {
                    return false;
                }
                refused += in_range;
                continue;
            }
            if (!in_range || index != (ticket - 1) * SEEDTOOL_TICKET_DIE + die - 1) {
                return false;
            }
            ++hits[index];
        }
    }
    for (size_t i = 0; i < SEEDTOOL_WORDLIST_LEN; ++i) {
        if (hits[i] != 1) {
            return false;
        }
    }
    if (refused != 4 || seedtool_ticket_word(1, 1, NULL) != SEEDTOOL_EINVAL) {
        return false;
    }

    /* And back: every word's ticket and die name that word again, which is
     * what the confirmation and the review show. */
    for (uint16_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        unsigned ticket = 0, die = 0;
        uint16_t again = 0;
        if (seedtool_word_ticket(index, &ticket, &die) != SEEDTOOL_OK
            || seedtool_ticket_word(ticket, die, &again) != SEEDTOOL_OK || again != index) {
            return false;
        }
    }
    unsigned ticket = 0, die = 0;
    if (seedtool_word_ticket(SEEDTOOL_WORDLIST_LEN, &ticket, &die) != SEEDTOOL_EINVAL
        || seedtool_word_ticket(0, NULL, &die) != SEEDTOOL_EINVAL) {
        return false;
    }

    static const uint16_t draws[23][2] = { { 57, 4 }, { 201, 1 }, { 342, 2 }, { 1, 1 }, { 118, 6 }, { 300, 3 },
        { 23, 5 }, { 171, 2 }, { 256, 4 }, { 89, 6 }, { 14, 3 }, { 330, 1 }, { 77, 2 }, { 142, 5 }, { 5, 6 },
        { 268, 1 }, { 199, 3 }, { 36, 4 }, { 311, 2 }, { 160, 6 }, { 44, 1 }, { 228, 5 }, { 97, 3 } };
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    size_t used = 0;
    for (size_t i = 0; i < 23; ++i) {
        uint16_t index = 0;
        if (seedtool_ticket_word(draws[i][0], draws[i][1], &index) != SEEDTOOL_OK) {
            return false;
        }
        used += (size_t)snprintf(prefix + used, sizeof(prefix) - used, "%s%s", i ? " " : "", seedtool_word(index));
    }
    static const char expected[] = "clerk noodle zoo abandon flash they baby leisure sausage dress anxiety walnut "
                                   "decrease heart admit sign network boy trim jewel cake print embark ";
    if (strncmp(prefix, expected, sizeof(expected) - 2) != 0) {
        return false;
    }
    const uint8_t tails[3] = { 0, 0, 0 }, mixed[3] = { 1, 0, 1 };
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
    char wanted[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
    (void)snprintf(wanted, sizeof(wanted), "%scabin", expected);
    if (seedtool_complete_checksum(prefix, tails, 3, completed, sizeof(completed)) != SEEDTOOL_OK
        || strcmp(completed, wanted) != 0) {
        return false;
    }
    (void)snprintf(wanted, sizeof(wanted), "%sphrase", expected);
    return seedtool_complete_checksum(prefix, mixed, 3, completed, sizeof(completed)) == SEEDTOOL_OK
        && strcmp(completed, wanted) == 0;
}

/* A die read word by word: every word comes from exactly as many rolls as
 * every other (two, since a word's last roll keeps only its first bits), so
 * none is favoured; faces that do not count and wrong roll counts are refused. */
static bool die_words_cover_the_wordlist(void)
{
    const seedtool_die_t dice[] = { SEEDTOOL_D6, SEEDTOOL_D20 };
    for (size_t d = 0; d < 2; ++d) {
        const seedtool_die_t die = dice[d];
        const unsigned faces = seedtool_die_faces(die);
        const size_t count = seedtool_die_rolls_per_word(die);
        if (count > SEEDTOOL_DIE_MAX_ROLLS) {
            return false;
        }
        unsigned hits[SEEDTOOL_WORDLIST_LEN] = { 0 };
        uint8_t rolls[SEEDTOOL_DIE_MAX_ROLLS];
        size_t total = 1;
        for (size_t r = 0; r < count; ++r) {
            total *= faces;
        }
        for (size_t n = 0; n < total; ++n) {
            size_t rest = n;
            for (size_t r = count; r-- > 0;) {
                rolls[r] = (uint8_t)(rest % faces + 1);
                rest /= faces;
            }
            uint16_t index = 0;
            if (seedtool_die_word(die, rolls, count, &index) != SEEDTOOL_OK) {
                return false;
            }
            ++hits[index];
        }
        for (size_t i = 0; i < SEEDTOOL_WORDLIST_LEN; ++i) {
            if (hits[i] != total / SEEDTOOL_WORDLIST_LEN) {
                return false;
            }
        }
        /* Each face that does not count, in each position, is refused. */
        for (size_t r = 0; r < count; ++r) {
            memset(rolls, 1, sizeof(rolls));
            uint16_t index = 0;
            rolls[r] = 0;
            if (seedtool_die_word(die, rolls, count, &index) != SEEDTOOL_EINVAL) {
                return false;
            }
            rolls[r] = (uint8_t)(faces + 1);
            if (seedtool_die_word(die, rolls, count, &index) != SEEDTOOL_EINVAL) {
                return false;
            }
        }
        uint16_t index = 0;
        if (seedtool_die_word(die, rolls, count - 1, &index) != SEEDTOOL_EINVAL
            || seedtool_die_word(die, NULL, count, &index) != SEEDTOOL_EINVAL) {
            return false;
        }
    }

    /* The table by hand: faces 1..4 are 00, 01, 10, 11, and the last roll
     * keeps its first bit. 4 4 4 4 4 4 is zoo, 4 4 4 4 4 2 is zone (2046:
     * the last 2 is 01, kept as 0), 1 2 3 4 1 3 is 00 01 10 11 00 1. */
    const uint8_t zoo[6] = { 4, 4, 4, 4, 4, 4 }, zone[6] = { 4, 4, 4, 4, 4, 2 }, mixed[6] = { 1, 2, 3, 4, 1, 3 };
    const uint8_t d20[3] = { 16, 1, 10 };
    uint16_t index = 0;
    return seedtool_die_word(SEEDTOOL_D6, zoo, 6, &index) == SEEDTOOL_OK && index == 2047
        && seedtool_die_word(SEEDTOOL_D6, zone, 6, &index) == SEEDTOOL_OK && index == 2046
        && seedtool_die_word(SEEDTOOL_D6, mixed, 6, &index) == SEEDTOOL_OK && index == 0x0D9
        /* 1111 0000 100: 15 << 7 | 0 << 3 | (9 >> 1). */
        && seedtool_die_word(SEEDTOOL_D20, d20, 3, &index) == SEEDTOOL_OK && index == (15u << 7 | 4u);
}

/* `count` words, all "abandon" when `spread` is zero and a spread of the
 * wordlist otherwise. An all-zero prefix cannot catch a packing offset, hence
 * the spread. */
static bool build_prefix(const size_t count, const unsigned spread, char* out, const size_t out_len)
{
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const char* const word
            = spread ? seedtool_word((size_t)((i * spread + 7u) % SEEDTOOL_WORDLIST_LEN)) : "abandon";
        const size_t len = strlen(word);
        if (used + len + (i ? 1 : 0) + 1 > out_len) {
            return false;
        }
        if (i) {
            out[used++] = ' ';
        }
        memcpy(out + used, word, len + 1);
        used += len;
    }
    return true;
}

/* The final-word filter must be exact both ways, for all 2048 words and both
 * lengths: a word left out must fail validation and a word left in must pass,
 * or a restorable seed becomes unrestorable. 12 words leave 2^7 = 128
 * candidates, 24 leave 2^3 = 8. */
/* How many words `set` holds. */
static size_t wordset_count(const seedtool_wordset_t* set)
{
    size_t count = 0;
    for (size_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        count += seedtool_wordset_has(set, index);
    }
    return count;
}

static bool final_word_filter_is_exact(void)
{
    static const struct {
        size_t words;
        size_t expected;
        unsigned spread;
    } cases[] = { { 12, 128, 0 }, { 24, 8, 0 }, { 12, 128, 173 }, { 24, 8, 409 } };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const size_t prefix_words = cases[c].words - 1;
        char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
        seedtool_wordset_t set;
        if (!build_prefix(prefix_words, cases[c].spread, prefix, sizeof(prefix))
            || seedtool_final_word_candidates(prefix, &set) != SEEDTOOL_OK
            || wordset_count(&set) != cases[c].expected) {
            return false;
        }
        for (uint16_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
            char full[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
            (void)snprintf(full, sizeof(full), "%s %s", prefix, seedtool_word(index));
            const bool valid = seedtool_validate_mnemonic(full, NULL) == SEEDTOOL_OK;
            if (valid != seedtool_wordset_has(&set, index)) {
                return false;
            }
        }
        /* Every word in the set stays reachable letter by letter, and no word
         * outside it can be picked at the end, even where it shares a prefix
         * with one inside ("above" and "about"). */
        for (uint16_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
            const char* const word = seedtool_word(index);
            const size_t length = strlen(word);
            uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
            bool letters[SEEDTOOL_LETTERS];
            const bool member = seedtool_wordset_has(&set, index);
            if (member) {
                for (size_t prefix_len = 0; prefix_len < length; ++prefix_len) {
                    if (!seedtool_next_letters_in(&set, word, prefix_len, letters)
                        || !letters[word[prefix_len] - 'a']) {
                        return false;
                    }
                }
            }
            /* Whether the word itself can be picked, not whether anything
             * matches its letters. */
            const size_t matches
                = seedtool_words_with_prefix_in(&set, word, length, candidates, SEEDTOOL_MAX_WORD_CHOICES);
            bool selectable = false;
            for (size_t i = 0; i < matches && i < SEEDTOOL_MAX_WORD_CHOICES; ++i) {
                selectable = selectable || candidates[i] == index;
            }
            if (selectable != member) {
                return false;
            }
            /* The number keyboard narrows the same way, so Accept stays dark
             * on a number the mnemonic cannot end with. */
            char digits[SEEDTOOL_MAX_WORD_DIGITS + 1];
            (void)snprintf(digits, sizeof(digits), "%04u", (unsigned)index + 1u);
            if ((seedtool_word_number_in(&set, digits, strlen(digits)) != 0) != member) {
                return false;
            }
        }
        /* A 24-word prefix leaves 8 words, few enough that entry lists them
         * outright instead of asking for a single letter. */
        if (cases[c].words == 24) {
            uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
            if (seedtool_words_with_prefix_in(&set, "", 0, candidates, SEEDTOOL_MAX_WORD_CHOICES)
                > SEEDTOOL_MAX_WORD_CHOICES) {
                return false;
            }
        }
    }

    /* A prefix that is not 11 or 23 words has no such thing as a final word. */
    seedtool_wordset_t set;
    return seedtool_final_word_candidates(mnemonic, &set) != SEEDTOOL_OK
        && seedtool_final_word_candidates("abandon", &set) != SEEDTOOL_OK;
}

/* Every reachable letter leads to a word, and narrowing always ends in a
 * listable candidate set. Checked exhaustively. */
static bool wordlist_completion_is_sound(void)
{
    uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
    bool enabled[SEEDTOOL_LETTERS];

    if (seedtool_words_with_prefix_in(NULL, "", 0, candidates, SEEDTOOL_MAX_WORD_CHOICES) != SEEDTOOL_WORDLIST_LEN
        || seedtool_next_letters_in(NULL, "", 0, enabled) == 0) {
        return false;
    }
    for (size_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        const char* const word = seedtool_word(index);
        const size_t length = strlen(word);
        if (!length || length > SEEDTOOL_MAX_WORD_LEN) {
            return false;
        }
        for (size_t prefix_len = 0; prefix_len < length; ++prefix_len) {
            /* Every prefix of a real word must match it, and the next letter of
             * that word must be offered by the keyboard. */
            if (!seedtool_words_with_prefix_in(NULL, word, prefix_len, candidates, SEEDTOOL_MAX_WORD_CHOICES)
                || !seedtool_next_letters_in(NULL, word, prefix_len, enabled) || !enabled[word[prefix_len] - 'a']) {
                return false;
            }
        }
        /* Typing four letters always narrows to a set the carousel can show. */
        const size_t stem = length < 4 ? length : 4;
        if (seedtool_words_with_prefix_in(NULL, word, stem, candidates, SEEDTOOL_MAX_WORD_CHOICES)
            > SEEDTOOL_MAX_WORD_CHOICES) {
            return false;
        }
    }
    return true;
}

/* Types `digits` one key at a time: every digit must be offered, accept must
 * stay off until the number is whole, and it must resolve to `expected`. */
static bool word_number_types_out(const char* digits, const size_t digits_len, const unsigned expected)
{
    bool enabled[SEEDTOOL_DIGITS];
    for (size_t typed = 0; typed < digits_len; ++typed) {
        if (!seedtool_next_digits_in(NULL, digits, typed, enabled) || !enabled[digits[typed] - '0']) {
            return false;
        }
        if (seedtool_word_number_in(NULL, digits, typed) != 0 && typed + 1 < digits_len
            && seedtool_word_number_in(NULL, digits, typed + 1) == 0) {
            /* A prefix that is a word number may only extend into another one. */
            return false;
        }
    }
    return seedtool_word_number_in(NULL, digits, digits_len) == expected && seedtool_word(expected - 1) != NULL;
}

/* All 2048 word numbers are typeable, plainly and padded to four digits. */
static bool word_numbers_are_reachable(void)
{
    for (unsigned number = 1; number <= SEEDTOOL_WORDLIST_LEN; ++number) {
        char plain[SEEDTOOL_MAX_WORD_DIGITS + 1], padded[SEEDTOOL_MAX_WORD_DIGITS + 1];
        const int length = snprintf(plain, sizeof(plain), "%u", number);
        if (length < 1 || snprintf(padded, sizeof(padded), "%04u", number) != SEEDTOOL_MAX_WORD_DIGITS) {
            return false;
        }
        if (!word_number_types_out(plain, (size_t)length, number)
            || !word_number_types_out(padded, SEEDTOOL_MAX_WORD_DIGITS, number)) {
            return false;
        }
    }
    /* Nothing outside 1..2048 may pass, however it is spelled. */
    static const char* const rejected[] = { "", "0", "00", "0000", "2049", "9999", "20480" };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        if (seedtool_word_number_in(NULL, rejected[i], strlen(rejected[i])) != 0) {
            return false;
        }
    }
    return true;
}

/* The other direction of the same convention: every word maps back to the
 * exact one-based number a Stackbit 1248 plate would be punched with. */
static bool word_numbers_round_trip_is_sound(void)
{
    for (unsigned index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        uint16_t numbers[1];
        size_t count = 0;
        if (seedtool_mnemonic_word_numbers(seedtool_word(index), numbers, 1, &count) != SEEDTOOL_OK || count != 1
            || numbers[0] != index + 1) {
            return false;
        }
    }
    uint16_t numbers[2];
    size_t count = 0;
    if (seedtool_mnemonic_word_numbers("abandon zoo", numbers, 2, &count) != SEEDTOOL_OK || count != 2
        || numbers[0] != 1 || numbers[1] != SEEDTOOL_WORDLIST_LEN) {
        return false;
    }
    return seedtool_mnemonic_word_numbers("abandon notaword", numbers, 2, &count) != SEEDTOOL_OK;
}

/* BIP380's published example (raw(deadbeef)#89f8spxm) and a wpkh() descriptor
 * over the BIP84 vector with BIP389's "<0;1>" step, cross-checked against an
 * independent Python transcription of the spec. Also EINVAL on a character
 * outside the charset and ENOSPACE on a short buffer. */
static bool descriptor_checksum_matches_bip380_vector(void)
{
    char out[256];
    if (seedtool_descriptor_checksum("raw(deadbeef)", out, sizeof(out)) != SEEDTOOL_OK
        || strcmp(out, "raw(deadbeef)#89f8spxm") != 0) {
        return false;
    }
    char body[192];
    (void)snprintf(body, sizeof(body), "wpkh([73c5da0a/84'/0'/0']%s/<0;1>/*)", expected_xpub84);
    if (seedtool_descriptor_checksum(body, out, sizeof(out)) != SEEDTOOL_OK) {
        return false;
    }
    char expected[256];
    (void)snprintf(expected, sizeof(expected), "%s#hpg6d6w2", body);
    if (strcmp(out, expected) != 0) {
        return false;
    }
    /* A raw newline is outside DESCRIPTOR_INPUT_CHARSET's structural
     * alphabet - checks the rejection path. */
    if (seedtool_descriptor_checksum("raw(dead\nbeef)", out, sizeof(out)) != SEEDTOOL_EINVAL) {
        return false;
    }
    char tiny[5];
    return seedtool_descriptor_checksum("raw(deadbeef)", tiny, sizeof(tiny)) == SEEDTOOL_ENOSPACE;
}

/* Vectors from Krux's base32 tests (tests/test_bbqr.py, unpadded), so the
 * output matches what BBQr readers already accept. */
static bool bbqr_base32_is_sound(void)
{
    static const struct {
        const uint8_t* data;
        size_t len;
        const char* encoded;
    } vectors[] = {
        { (const uint8_t*)"Hello World", 11, "JBSWY3DPEBLW64TMMQ" },
        { (const uint8_t*)"Hello World.", 12, "JBSWY3DPEBLW64TMMQXA" },
        { (const uint8_t*)"1234567890", 10, "GEZDGNBVGY3TQOJQ" },
        { (const uint8_t*)"\x00", 1, "AA" },
        { (const uint8_t*)"f", 1, "MY" },
        { (const uint8_t*)"\x01\x02\x03\x04", 4, "AEBAGBA" },
        { (const uint8_t*)"\x00\xff\xfe\xfd\xfc\xfb", 6, "AD7757P47M" },
        { (const uint8_t*)"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 10, "AAAAAAAAAAAAAAAA" },
        { (const uint8_t*)"\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 10, "7777777777777777" },
        { (const uint8_t*)"Hello, World!", 13, "JBSWY3DPFQQFO33SNRSCC" },
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        char out[32];
        const size_t expected_len = strlen(vectors[i].encoded);
        if (seedtool_bbqr_base32_len(vectors[i].len) != expected_len) {
            return false;
        }
        if (!seedtool_bbqr_base32_encode(vectors[i].data, vectors[i].len, out, sizeof(out))) {
            return false;
        }
        if (memcmp(out, vectors[i].encoded, expected_len) != 0) {
            return false;
        }
    }
    return true;
}

/* The vector mnemonic's master key, with no passphrase. */
static seedtool_root_t vector_root(void)
{
    seedtool_root_t root;
    if (seedtool_root_from_mnemonic(mnemonic, "", &root) != SEEDTOOL_OK) {
        memset(&root, 0, sizeof(root));
    }
    return root;
}

/* One address, through the batch derivation the firmware uses. */
static seedtool_result_t address_at(const seedtool_address_type_t type, const seedtool_chain_t chain,
    const uint32_t index, char output[SEEDTOOL_MAX_ADDRESS_LEN])
{
    char row[1][SEEDTOOL_MAX_ADDRESS_LEN];
    const seedtool_root_t root = vector_root();
    const seedtool_result_t ret = seedtool_mainnet_addresses(&root, type, 0, chain, index, 1, row);
    if (ret == SEEDTOOL_OK) {
        memcpy(output, row[0], SEEDTOOL_MAX_ADDRESS_LEN);
    }
    return ret;
}

/* The published BIP84 and BIP86 vectors, and SLIP-132's zpub for the same
 * account. Taproot has no zpub. */
static bool published_vectors_match(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    seedtool_root_t root;
    CHECK(seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK);
    CHECK(seedtool_root_from_mnemonic(mnemonic, "", &root) == SEEDTOOL_OK);
    CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_RECEIVE, 0, address) == SEEDTOOL_OK);
    CHECK(strcmp(address, expected_address) == 0);
    CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_CHANGE, 0, address) == SEEDTOOL_OK);
    CHECK(strcmp(address, expected_change_address) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_xpub84) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP86, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_xpub86) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_zpub84) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP86, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub))
        == SEEDTOOL_EINVAL);
    return true;
}

/* Legacy and nested segwit: the published first receive addresses, the account
 * keys, and SLIP-132 offered only where it is defined - ypub for BIP49, zpub
 * for BIP84, neither for BIP44. */
static bool legacy_and_nested_vectors_match(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    const seedtool_root_t root = vector_root();
    CHECK(address_at(SEEDTOOL_BIP44, SEEDTOOL_RECEIVE, 0, address) == SEEDTOOL_OK);
    CHECK(strcmp(address, expected_address44) == 0);
    CHECK(address_at(SEEDTOOL_BIP49, SEEDTOOL_RECEIVE, 0, address) == SEEDTOOL_OK);
    CHECK(strcmp(address, expected_address49) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP44, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_xpub44) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP49, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_xpub49) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP49, 0, SEEDTOOL_YPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_ypub49) == 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP44, 0, SEEDTOOL_YPUB, xpub, sizeof(xpub)) == SEEDTOOL_EINVAL);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP44, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) == SEEDTOOL_EINVAL);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP49, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) == SEEDTOOL_EINVAL);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_YPUB, xpub, sizeof(xpub)) == SEEDTOOL_EINVAL);
    CHECK(seedtool_account_xpub(&root, (seedtool_address_type_t)45, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub))
        == SEEDTOOL_EINVAL);
    return true;
}

/* The account and chain are really threaded into the path - different values
 * give different keys - and both are bounded rather than taken on trust. */
static bool account_and_chain_reach_the_path(void)
{
    char zero[SEEDTOOL_MAX_XPUB_LEN];
    char one[SEEDTOOL_MAX_XPUB_LEN];
    const seedtool_root_t root = vector_root();
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_XPUB, zero, sizeof(zero)) == SEEDTOOL_OK);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP84, 1, SEEDTOOL_XPUB, one, sizeof(one)) == SEEDTOOL_OK);
    CHECK(strcmp(zero, one) != 0);
    CHECK(seedtool_account_xpub(&root, SEEDTOOL_BIP84, SEEDTOOL_MAX_ACCOUNT_INDEX + 1, SEEDTOOL_XPUB, one, sizeof(one))
        == SEEDTOOL_EINVAL);

    char receive[SEEDTOOL_MAX_ADDRESS_LEN];
    char change[SEEDTOOL_MAX_ADDRESS_LEN];
    char batch[2][SEEDTOOL_MAX_ADDRESS_LEN];
    CHECK(address_at(SEEDTOOL_BIP86, SEEDTOOL_RECEIVE, 3, receive) == SEEDTOOL_OK);
    CHECK(address_at(SEEDTOOL_BIP86, SEEDTOOL_CHANGE, 3, change) == SEEDTOOL_OK);
    CHECK(strcmp(receive, change) != 0);
    CHECK(address_at(SEEDTOOL_BIP84, (seedtool_chain_t)2, 0, receive) == SEEDTOOL_EINVAL);
    CHECK(seedtool_mainnet_addresses(&root, SEEDTOOL_BIP84, 0, (seedtool_chain_t)2, 0, 2, batch)
        == SEEDTOOL_EINVAL);
    return true;
}

/* A batch starting past zero is the same addresses as one at a time, the last
 * index is reachable, and nothing past it is. Go to index derives one address
 * at a time, the list a batch from zero, and the two must agree. */
static bool address_ranges_are_consistent(void)
{
    const seedtool_root_t root = vector_root();
    char batch[3][SEEDTOOL_MAX_ADDRESS_LEN];
    char one[SEEDTOOL_MAX_ADDRESS_LEN];
    CHECK(seedtool_mainnet_addresses(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_RECEIVE, 48, 3, batch) == SEEDTOOL_OK);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_RECEIVE, 48 + i, one) == SEEDTOOL_OK);
        CHECK(strcmp(one, batch[i]) == 0);
    }
    CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_RECEIVE, SEEDTOOL_MAX_ADDRESS_INDEX, one) == SEEDTOOL_OK);
    CHECK(strcmp(one, expected_address_9999) == 0);
    CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_RECEIVE, SEEDTOOL_MAX_ADDRESS_INDEX + 1, one) == SEEDTOOL_EINVAL);
    CHECK(seedtool_mainnet_addresses(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_RECEIVE, SEEDTOOL_MAX_ADDRESS_INDEX, 2, batch)
        == SEEDTOOL_EINVAL);
    CHECK(seedtool_mainnet_addresses(&root, SEEDTOOL_BIP84, 0, SEEDTOOL_RECEIVE, 0, 0, batch) == SEEDTOOL_EINVAL);
    return true;
}

/* The fingerprint the BIP84 vector's key origin states, and one that moves
 * with the passphrase: the wallet title is the reader's only sign of which
 * seed is in force. */
static bool fingerprint_follows_the_passphrase(void)
{
    static const uint8_t expected[4] = { 0x73, 0xc5, 0xda, 0x0a };
    uint8_t fp[4], with_passphrase[4];
    seedtool_root_t root;
    CHECK(seedtool_root_from_mnemonic(mnemonic, "", &root) == SEEDTOOL_OK);
    CHECK(seedtool_master_fingerprint(&root, fp) == SEEDTOOL_OK);
    CHECK(memcmp(fp, expected, sizeof(fp)) == 0);
    CHECK(seedtool_root_from_mnemonic(mnemonic, "TREZOR", &root) == SEEDTOOL_OK);
    CHECK(seedtool_master_fingerprint(&root, with_passphrase) == SEEDTOOL_OK);
    CHECK(memcmp(fp, with_passphrase, sizeof(fp)) != 0);
    CHECK(seedtool_root_from_mnemonic(mnemonic, "tab\there", &root) == SEEDTOOL_EINVAL);
    CHECK(seedtool_root_from_mnemonic("abandon abandon", "", &root) == SEEDTOOL_EINVAL);
    return true;
}

int main(void)
{
    if (wally_init(0) != WALLY_OK) {
        return 1;
    }
    static const test_t tests[] = {
        TEST(coin_word_encoding_is_sound),
        TEST(ticket_words_cover_the_wordlist),
        TEST(die_words_cover_the_wordlist),
        TEST(final_word_filter_is_exact),
        TEST(wordlist_completion_is_sound),
        TEST(word_numbers_are_reachable),
        TEST(word_numbers_round_trip_is_sound),
        TEST(descriptor_checksum_matches_bip380_vector),
        TEST(bbqr_base32_is_sound),
        TEST(published_vectors_match),
        TEST(account_and_chain_reach_the_path),
        TEST(legacy_and_nested_vectors_match),
        TEST(fingerprint_follows_the_passphrase),
        TEST(address_ranges_are_consistent),
    };
    const int ret = run_tests("core", tests, sizeof(tests) / sizeof(tests[0]));
    wally_cleanup(0);
    return ret;
}
