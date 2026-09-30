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

/* Eleven flips name one word, most significant bit first, and the words they
 * name are handed to seedtool_complete_checksum - the same call the typed-word
 * flow makes. The screen that shows the reader "these eleven flips are word
 * 0819" is only telling the truth if that index survives into the finished
 * mnemonic unchanged, so that is what is checked, for every one of the 2048
 * words rather than for a chosen few.
 *
 * The all-zero case is pinned to the published vectors the rest of this file
 * already uses: eleven words of index 0 plus seven zero bits are 128 zero
 * entropy bits, and twenty-three plus three zero bits are 256 - `mnemonic` and
 * `mnemonic24`. That is the one case where the whole chain, checksum included,
 * is fixed by something outside this project. */
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

    /* An eleven-bit index is the whole wordlist and nothing past it: the top
     * index must be reachable, and the encoding must not overflow into a
     * twelfth bit. */
    return SEEDTOOL_WORDLIST_LEN == (1u << 11);
}

/* `count` words, all "abandon" when `spread` is zero and a spread of the
 * wordlist otherwise.
 *
 * The all-"abandon" prefix is the one the published vectors use, but every one
 * of its word indices is zero - so every bit it contributes to the entropy is
 * zero too, and a prefix packed at the wrong bit offset would still hash to the
 * right thing. It cannot catch a packing bug on its own, which is why the
 * checks below run over a spread prefix as well. */
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

/* The last word of a mnemonic carries the checksum, so most of the wordlist
 * cannot end it. The entry keyboard narrows to the ones that can, which is only
 * safe if the narrowing is *exact*: a word left out must genuinely fail
 * seedtool_validate_mnemonic, and a word left in must genuinely pass. Both
 * directions are checked against every one of the 2048 words, for a 12-word
 * prefix and a 24-word one, because a filter that is merely nearly right would
 * turn a restorable seed into an unrestorable one - a far worse failure than
 * the mistyped checksum it exists to prevent.
 *
 * The counts are arithmetic, not observation: 12 words leave 7 free entropy
 * bits before 4 checksum bits, so 2^7 = 128 words can end them; 24 words leave
 * 3 before 8, so only 8 can. */
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
        /* Every word the set holds must stay reachable letter by letter, and
         * no word outside it may be selectable at the end. Those are two
         * different claims: a word outside the set can share a prefix with one
         * inside it - "above" is not a candidate but starts like "about",
         * which is - so reachability of a prefix says nothing on its own. What
         * must not happen is arriving at the whole word and being able to pick
         * it. */
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
             * matches its letters: "act" is not a candidate but is a prefix of
             * "action", which may be, so a match count says nothing here. What
             * entry offers is the candidate list, so the question is whether
             * this index is in it. */
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

/* The word-entry keyboard is only usable if every reachable letter really does
 * lead to a word, and if narrowing always terminates in a listable candidate
 * set. Both are checked exhaustively rather than by example. */
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

/* Type `digits` one key at a time and require that the keyboard offered every
 * digit along the way, that accept stayed off until the number was whole, and
 * that the number then resolves to `expected`. */
static bool word_number_types_out(const char* digits, const size_t digits_len, const unsigned expected)
{
    bool enabled[SEEDTOOL_DIGITS];
    for (size_t typed = 0; typed < digits_len; ++typed) {
        if (!seedtool_next_digits_in(NULL, digits, typed, enabled) || !enabled[digits[typed] - '0']) {
            return false;
        }
        if (seedtool_word_number_in(NULL, digits, typed) != 0 && typed + 1 < digits_len
            && seedtool_word_number_in(NULL, digits, typed + 1) == 0) {
            /* A prefix that is itself a word number may only be extended into
             * another one, or accept would be offered for a number the user is
             * still in the middle of typing. */
            return false;
        }
    }
    return seedtool_word_number_in(NULL, digits, digits_len) == expected && seedtool_word(expected - 1) != NULL;
}

/* Numbers are the whole input for a backup that records them, so every one of
 * the 2048 has to be typeable, both plainly and padded to four digits the way a
 * printed wordlist prints it. */
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

/* seedtool_generate must actually succeed at every source/word-count pairing
 * this app offers, run at the *real* seedtool_required_events count - not a
 * smaller stand-in. This is what caught SEEDTOOL_MAX_TRANSCRIPT_LEN being too
 * small for a full 256-flip Coin/24-word transcript: partial_transcripts_are_
 * prefixes above only ever exercises up to 40 events, so it structurally
 * could not have caught a bug that only bites near the true maximum. */
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

/* Checked against BIP380's own published example (bip-0380.mediawiki,
 * "Checksum" section: raw(deadbeef) -> raw(deadbeef)#89f8spxm) rather than a
 * vector this project invented for itself, plus a real wpkh() descriptor
 * built from this file's own published BIP84 xpub vector, its checksum
 * cross-checked independently against a from-scratch Python transcription of
 * the same bip-0380.mediawiki pseudocode before this C version was written -
 * two independent implementations of the spec agreeing is the actual
 * confidence here, not either one alone. The descriptor carries BIP389's
 * multipath "<0;1>" chain step, which is exactly what show_descriptor writes:
 * "<", ";" and ">" are all in BIP380's INPUT_CHARSET, so this also pins that
 * the checksum covers a multipath descriptor rather than rejecting one. Also
 * checks EINVAL on a character outside the checksum's charset and ENOSPACE on
 * a buffer too small for the result. */
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

/* collect_entropy() gathers a run into values[256], entropy_quality() copies
 * it into faces[256], and seedtool_generate() renders it into the
 * SEEDTOOL_MAX_TRANSCRIPT_LEN-wide transcript field of seedtool_generated_t.
 * None of the three is bounds-checked - the collection loop is simply
 * `while (i < required)` - so they stay in range only because
 * seedtool_required_events happens never to hand out more than they hold.
 * The margin is not comfortable: coin at 24 words fills the transcript to the
 * last byte. Nothing enforced that before this check, so a future count could
 * cross the line silently and write past a stack array on a device with a
 * live seed in memory.
 *
 * Both ceilings are exercised, because they bind different sources: the event
 * count limits coin, while the rendered length limits D20, which spends three
 * characters on a roll that costs the array one byte. */
/* The compile-time half of this lives in seedtool_core.h, beside
 * SEEDTOOL_MAX_EVENTS itself, so it binds the firmware rather than only this
 * test binary. What is left for runtime is the half a C compiler cannot see:
 * seedtool_required_events' returns are function results, not constant
 * expressions, so every source and word count has to be asked. */
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
            /* The widest transcript the source can print at that length: the
             * highest face of a die, and distinct cards for the deck that
             * rejects a repeat. */
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

/* Known-good vectors straight from Krux's own base32 test suite
 * (tests/test_bbqr.py, B32_TEST_BYTES/B32_ENCODED_STRINGS, unpadded form):
 * proof this encoder produces byte-for-byte the same output a BBQr-reading
 * wallet already interoperates with, not just an internally self-consistent
 * one. */
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
