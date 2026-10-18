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
            || seedtool_wordset_count(&set) != cases[c].expected) {
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

/* seedtool_generate succeeds for every source and word count at the real
 * required event count, which is where a too-small buffer would show. */
static bool full_entropy_events_generate(void)
{
    static const seedtool_source_t sources[]
        = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_COIN, SEEDTOOL_CARDS, SEEDTOOL_CARDS_REPLACE };
    static const size_t word_counts[] = { 12, 24 };
    uint8_t values[SEEDTOOL_MAX_EVENTS];
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s) {
        for (size_t w = 0; w < sizeof(word_counts) / sizeof(word_counts[0]); ++w) {
            const size_t words = word_counts[w];
            const size_t count = seedtool_required_events(sources[s], words);
            if (!count) {
                /* Not every source offers every word count (Cards doesn't do
                 * 24, Cards-with-replacement doesn't do 12) - skip those. */
                continue;
            }
            for (size_t i = 0; i < count; ++i) {
                switch (sources[s]) {
                case SEEDTOOL_D6:
                    values[i] = (uint8_t)(1 + i % 6);
                    break;
                case SEEDTOOL_D20:
                    values[i] = (uint8_t)(1 + i % 20);
                    break;
                case SEEDTOOL_COIN:
                    values[i] = (uint8_t)(i % 2);
                    break;
                case SEEDTOOL_CARDS:
                    /* No repeats allowed; count never exceeds the 52-card
                     * deck, so a straight index is always distinct. */
                    values[i] = (uint8_t)i;
                    break;
                case SEEDTOOL_CARDS_REPLACE:
                    values[i] = (uint8_t)(i % 5);
                    break;
                default:
                    values[i] = 0;
                    break;
                }
            }
            seedtool_generated_t generated;
            const seedtool_result_t ret = seedtool_generate(sources[s], words, values, count, &generated);
            seedtool_zero(&generated, sizeof(generated));
            if (ret != SEEDTOOL_OK) {
                return false;
            }
        }
    }
    seedtool_zero(values, sizeof(values));
    return true;
}

/* A pairing with no required count must not generate from an empty run, and a
 * rejected run must not leave its transcript behind. */
static bool invalid_runs_do_not_generate(void)
{
    const uint8_t values[] = { 1 };
    const uint8_t bad[] = { 1, 2, 7 };
    seedtool_generated_t generated;
    char transcript[8];
    memset(&generated, 0xa5, sizeof(generated));
    if (seedtool_generate(SEEDTOOL_CARDS, 24, values, 0, &generated) != SEEDTOOL_EINVAL
        || seedtool_generate(SEEDTOOL_CARDS_REPLACE, 12, values, 0, &generated) != SEEDTOOL_EINVAL
        || seedtool_generate(SEEDTOOL_D6, 13, values, 0, &generated) != SEEDTOOL_EINVAL
        || seedtool_transcript(SEEDTOOL_D6, values, 1, NULL, 8) != SEEDTOOL_EINVAL
        || seedtool_transcript(SEEDTOOL_D6, values, 1, transcript, 0) != SEEDTOOL_EINVAL
        || seedtool_transcript(SEEDTOOL_D6, NULL, 1, transcript, sizeof(transcript)) != SEEDTOOL_EINVAL) {
        return false;
    }
    uint8_t run[60];
    memset(run, 1, sizeof(run));
    memcpy(run, bad, sizeof(bad));
    if (seedtool_generate(SEEDTOOL_D6, 12, run, sizeof(run), &generated) != SEEDTOOL_EINVAL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(generated); ++i) {
        if (((const uint8_t*)&generated)[i]) {
            return false;
        }
    }
    return true;
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

/* Every buffer a run passes through (values[], faces[] and the transcript) is
 * sized by SEEDTOOL_MAX_EVENTS or SEEDTOOL_MAX_TRANSCRIPT_LEN and not checked
 * again, so every source's count must fit both: coin binds the event count, D20
 * the transcript length. */
/* The compile-time half is in seedtool_core.h; the counts are function results,
 * so this asks every source and word count. */
static bool required_events_fit_the_collection_buffers(void)
{
    const seedtool_source_t sources[]
        = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_COIN, SEEDTOOL_CARDS, SEEDTOOL_CARDS_REPLACE };
    const size_t words[] = { 12, 24 };
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s) {
        for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); ++w) {
            const size_t required = seedtool_required_events(sources[s], words[w]);
            if (!required) {
                continue;
            }
            if (required > SEEDTOOL_MAX_EVENTS) {
                return false;
            }
            /* The widest transcript a source can print: its highest face, or
             * distinct cards. */
            uint8_t values[SEEDTOOL_MAX_EVENTS];
            for (size_t i = 0; i < required; ++i) {
                values[i] = sources[s] == SEEDTOOL_D6 ? 6
                    : sources[s] == SEEDTOOL_D20      ? 20
                    : sources[s] == SEEDTOOL_COIN     ? 1
                                                      : (uint8_t)(i % 52);
            }
            char transcript[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1];
            if (seedtool_transcript(sources[s], values, required, transcript, sizeof(transcript)) != SEEDTOOL_OK) {
                return false;
            }
        }
    }
    return true;
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

/* One address, through the batch derivation the firmware uses. */
static seedtool_result_t address_at(const seedtool_address_type_t type, const seedtool_chain_t chain,
    const uint32_t index, char output[SEEDTOOL_MAX_ADDRESS_LEN])
{
    char rows[SEEDTOOL_MAX_ADDRESS_INDEX + 1][SEEDTOOL_MAX_ADDRESS_LEN];
    const seedtool_result_t ret = seedtool_mainnet_addresses(mnemonic, "", type, 0, chain, index + 1, rows);
    if (ret == SEEDTOOL_OK) {
        memcpy(output, rows[index], SEEDTOOL_MAX_ADDRESS_LEN);
    }
    return ret;
}

/* The published BIP84 and BIP86 vectors, and SLIP-132's zpub for the same
 * account. Taproot has no zpub. */
static bool published_vectors_match(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    CHECK(seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK);
    CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_RECEIVE, 0, address) == SEEDTOOL_OK);
    CHECK(strcmp(address, expected_address) == 0);
    CHECK(address_at(SEEDTOOL_BIP84, SEEDTOOL_CHANGE, 0, address) == SEEDTOOL_OK);
    CHECK(strcmp(address, expected_change_address) == 0);
    CHECK(seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_xpub84) == 0);
    CHECK(seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_xpub86) == 0);
    CHECK(seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) == SEEDTOOL_OK);
    CHECK(strcmp(xpub, expected_zpub84) == 0);
    CHECK(seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub))
        == SEEDTOOL_EINVAL);
    return true;
}

/* The account and chain are really threaded into the path - different values
 * give different keys - and both are bounded rather than taken on trust. */
static bool account_and_chain_reach_the_path(void)
{
    char zero[SEEDTOOL_MAX_XPUB_LEN];
    char one[SEEDTOOL_MAX_XPUB_LEN];
    CHECK(seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_XPUB, zero, sizeof(zero)) == SEEDTOOL_OK);
    CHECK(seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 1, SEEDTOOL_XPUB, one, sizeof(one)) == SEEDTOOL_OK);
    CHECK(strcmp(zero, one) != 0);
    CHECK(seedtool_account_xpub(
              mnemonic, "", SEEDTOOL_BIP84, SEEDTOOL_MAX_ACCOUNT_INDEX + 1, SEEDTOOL_XPUB, one, sizeof(one))
        == SEEDTOOL_EINVAL);

    char receive[SEEDTOOL_MAX_ADDRESS_LEN];
    char change[SEEDTOOL_MAX_ADDRESS_LEN];
    char batch[2][SEEDTOOL_MAX_ADDRESS_LEN];
    CHECK(address_at(SEEDTOOL_BIP86, SEEDTOOL_RECEIVE, 3, receive) == SEEDTOOL_OK);
    CHECK(address_at(SEEDTOOL_BIP86, SEEDTOOL_CHANGE, 3, change) == SEEDTOOL_OK);
    CHECK(strcmp(receive, change) != 0);
    CHECK(address_at(SEEDTOOL_BIP84, (seedtool_chain_t)2, 0, receive) == SEEDTOOL_EINVAL);
    CHECK(seedtool_mainnet_addresses(mnemonic, "", SEEDTOOL_BIP84, 0, (seedtool_chain_t)2, 2, batch)
        == SEEDTOOL_EINVAL);
    return true;
}

int main(void)
{
    if (wally_init(0) != WALLY_OK) {
        return 1;
    }
    static const test_t tests[] = {
        TEST(coin_word_encoding_is_sound),
        TEST(final_word_filter_is_exact),
        TEST(wordlist_completion_is_sound),
        TEST(word_numbers_are_reachable),
        TEST(word_numbers_round_trip_is_sound),
        TEST(full_entropy_events_generate),
        TEST(invalid_runs_do_not_generate),
        TEST(descriptor_checksum_matches_bip380_vector),
        TEST(required_events_fit_the_collection_buffers),
        TEST(bbqr_base32_is_sound),
        TEST(published_vectors_match),
        TEST(account_and_chain_reach_the_path),
    };
    const int ret = run_tests("core", tests, sizeof(tests) / sizeof(tests[0]));
    wally_cleanup(0);
    return ret;
}
