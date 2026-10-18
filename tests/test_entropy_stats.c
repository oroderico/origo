/* Monte Carlo checks of the entropy quality signal against honest and
 * patterned runs. Slow, and statistical rather than exact. */
#include "check.h"

#include "seedtool_core.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

/* Vectors for the entropy quality readout, adapted from Krux. A UI signal: none
 * of this reaches the seed. */
static bool dice_quality_is_sound(void)
{
    uint8_t all_same[50];
    memset(all_same, 3, sizeof(all_same));
    int bits = -1;
    bool pattern = false;
    /* No information at all: every roll was foretold by the last. */
    if (seedtool_dice_entropy_bits(SEEDTOOL_D6, all_same, sizeof(all_same), &bits) != SEEDTOOL_OK || bits != 0) {
        return false;
    }
    if (seedtool_dice_pattern_detected(SEEDTOOL_D6, all_same, sizeof(all_same), &pattern) != SEEDTOOL_OK || !pattern) {
        return false;
    }

    /* A round-robin: every face equally often, but predictable, which only the
     * pattern check catches. */
    uint8_t round_robin[50];
    for (size_t i = 0; i < sizeof(round_robin); ++i) {
        round_robin[i] = (uint8_t)((i % 6) + 1);
    }
    if (seedtool_dice_entropy_bits(SEEDTOOL_D6, round_robin, sizeof(round_robin), &bits) != SEEDTOOL_OK
        || bits < (int)seedtool_min_entropy_bits(12)) {
        return false;
    }
    if (seedtool_dice_pattern_detected(SEEDTOOL_D6, round_robin, sizeof(round_robin), &pattern) != SEEDTOOL_OK
        || !pattern) {
        return false;
    }

    /* Too few rolls to judge a pattern either way. */
    const uint8_t short_run[9] = { 1, 2, 3, 4, 5, 6, 1, 2, 3 };
    if (seedtool_dice_pattern_detected(SEEDTOOL_D6, short_run, sizeof(short_run), &pattern) != SEEDTOOL_OK
        || pattern) {
        return false;
    }

    /* A coin is a two-sided die, 1-indexed like every other source here -
     * not the 0/1 seedtool_transcript stores. All-heads: no information. */
    const uint8_t coin_all_heads[10] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
    if (seedtool_dice_entropy_bits(SEEDTOOL_COIN, coin_all_heads, sizeof(coin_all_heads), &bits) != SEEDTOOL_OK
        || bits != 0) {
        return false;
    }
    if (seedtool_dice_pattern_detected(SEEDTOOL_COIN, coin_all_heads, sizeof(coin_all_heads), &pattern) != SEEDTOOL_OK
        || !pattern) {
        return false;
    }

    /* Cards are not a die - see card_quality_is_sound below. */
    const uint8_t card = 0;
    return seedtool_dice_entropy_bits(SEEDTOOL_CARDS, &card, 1, &bits) == SEEDTOOL_EINVAL;
}

/* Card draws: the bits are exact, so this checks the boundaries and that the
 * count never decreases. */
static bool card_quality_is_sound(void)
{
    int bits = -1;
    if (seedtool_card_entropy_bits(0, &bits) != SEEDTOOL_OK || bits != 0) {
        return false;
    }
    /* 12-word cards clear 128 bits by construction. */
    if (seedtool_card_entropy_bits(25, &bits) != SEEDTOOL_OK || bits < (int)seedtool_min_entropy_bits(12)) {
        return false;
    }
    /* Non-decreasing over the deck; only the last card, fixed by the other 51,
     * adds nothing. */
    int previous = 0;
    for (size_t drawn = 1; drawn <= 52; ++drawn) {
        if (seedtool_card_entropy_bits(drawn, &bits) != SEEDTOOL_OK || bits < previous) {
            return false;
        }
        previous = bits;
    }
    if (seedtool_card_entropy_bits(53, &bits) != SEEDTOOL_EINVAL) {
        return false;
    }

    /* An unshuffled deck in order: ranks count up within each suit, the pattern
     * the check exists for. */
    uint8_t ascending[25];
    for (size_t i = 0; i < sizeof(ascending); ++i) {
        ascending[i] = (uint8_t)i;
    }
    bool pattern = false;
    if (seedtool_card_pattern_detected(ascending, sizeof(ascending), &pattern) != SEEDTOOL_OK || !pattern) {
        return false;
    }

    /* Too few draws to judge a pattern either way. */
    const uint8_t short_run[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    return seedtool_card_pattern_detected(short_run, sizeof(short_run), &pattern) == SEEDTOOL_OK && !pattern;
}

/* xorshift32: a tiny, deterministic, portable PRNG (not libc rand(), which is
 * platform-specific) used only to drive the Monte-Carlo check below. */
static uint32_t xorshift32(uint32_t* state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* The poor-entropy gate corrects the estimator's small-sample bias
 * (seedtool_dice_entropy_bias_bits). This proves honest runs at the required
 * counts are rarely flagged. DICE_ENTROPY_TOLERANCE is copied from
 * seedtool_app.c, being UI policy. */
static bool dice_entropy_false_positive_rate_is_bounded(void)
{
    /* Coin at its required count, which is exactly its minimum, and
     * SEEDTOOL_CARDS_REPLACE at whatever count it asks for. */
    const seedtool_source_t sources[] = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_COIN, SEEDTOOL_CARDS_REPLACE };
    const size_t sides[] = { 6, 20, 2, 52 };
    const size_t words[] = { 12, 24 };
    const int dice_entropy_tolerance = 4; /* DICE_ENTROPY_TOLERANCE in main/seedtool_app.c */
    const size_t trials = 500;
    const double max_false_positive_rate = 0.10;

    uint32_t state = 1;
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s) {
        const double bias = seedtool_dice_entropy_bias_bits(sources[s]);
        for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); ++w) {
            const size_t required = seedtool_required_events(sources[s], words[w]);
            if (!required) {
                continue; /* this source/word-count combination is not offered */
            }
            const size_t min_bits = seedtool_min_entropy_bits(words[w]);
            size_t flagged = 0;
            for (size_t t = 0; t < trials; ++t) {
                uint8_t values[SEEDTOOL_MAX_EVENTS];
                for (size_t i = 0; i < required; ++i) {
                    values[i] = (uint8_t)(xorshift32(&state) % sides[s]) + 1;
                }
                int bits = 0;
                if (seedtool_dice_entropy_bits(sources[s], values, required, &bits) != SEEDTOOL_OK) {
                    return false;
                }
                const int corrected = bits + (int)lround(bias);
                if ((size_t)corrected + dice_entropy_tolerance < min_bits) {
                    ++flagged;
                }
            }
            if ((double)flagged / (double)trials > max_false_positive_rate) {
                return false;
            }
        }
    }
    return true;
}

/* An honest run must also read at or above the minimum on screen, a stricter
 * property than not being flagged. Coin is excluded: one flip is one bit, so
 * its count is both the minimum and the whole buffer, and some honest runs read
 * a bit short (see the README's entropy section). */
static bool honest_runs_report_at_least_the_minimum(void)
{
    const seedtool_source_t sources[] = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_CARDS_REPLACE };
    const size_t sides[] = { 6, 20, 52 };
    const size_t words[] = { 12, 24 };
    const size_t trials = 2000;
    const double max_short_read_rate = 0.01;

    /* Its own stream, so adding this check cannot shift the draws the
     * false-positive bound above already passes against. */
    uint32_t state = 20260809;
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s) {
        const double bias = seedtool_dice_entropy_bias_bits(sources[s]);
        for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); ++w) {
            const size_t required = seedtool_required_events(sources[s], words[w]);
            if (!required) {
                continue; /* this source/word-count combination is not offered */
            }
            const int min_bits = (int)seedtool_min_entropy_bits(words[w]);
            size_t short_reads = 0;
            for (size_t t = 0; t < trials; ++t) {
                uint8_t values[SEEDTOOL_MAX_EVENTS];
                for (size_t i = 0; i < required; ++i) {
                    values[i] = (uint8_t)(xorshift32(&state) % sides[s]) + 1;
                }
                int bits = 0;
                if (seedtool_dice_entropy_bits(sources[s], values, required, &bits) != SEEDTOOL_OK) {
                    return false;
                }
                if (bits + (int)lround(bias) < min_bits) {
                    ++short_reads;
                }
            }
            if ((double)short_reads / (double)trials > max_short_read_rate) {
                return false;
            }
        }
    }
    return true;
}

/* The other direction: a source degraded so far that its run truly carries
 * fewer bits than the seed needs must be flagged. Each case keeps `count *
 * log2(usable)` below the minimum, and the expected verdict is computed from
 * it. */
static bool insufficient_runs_are_flagged(void)
{
    static const struct {
        seedtool_source_t source;
        size_t usable; /* distinct values the degraded source can produce */
        size_t words;
    } cases[] = {
        { SEEDTOOL_D6, 3, 12 }, /* 60 * log2(3) = 95 bits against 128 */
        { SEEDTOOL_D6, 4, 12 }, /* 60 * 2 = 120 against 128 */
        { SEEDTOOL_D6, 4, 24 }, /* 120 * 2 = 240 against 256 */
        { SEEDTOOL_D20, 8, 12 }, /* 36 * 3 = 108 against 128 */
        { SEEDTOOL_D20, 8, 24 }, /* 68 * 3 = 204 against 256 */
        { SEEDTOOL_COIN, 1, 12 }, /* a two-headed coin: 0 bits */
        { SEEDTOOL_CARDS_REPLACE, 8, 24 }, /* 50 * 3 = 150 against 256 */
    };
    const int dice_entropy_tolerance = 4; /* DICE_ENTROPY_TOLERANCE in main/seedtool_app.c */
    const size_t trials = 500;
    const double min_detection_rate = 0.99;

    uint32_t state = 13579;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        const size_t required = seedtool_required_events(cases[c].source, cases[c].words);
        if (!required) {
            return false; /* every case above is a combination the app offers */
        }
        const int min_bits = (int)seedtool_min_entropy_bits(cases[c].words);
        /* Refuse to test a case that is not actually insufficient - that would
         * be asserting the gate flags a run it ought to pass. */
        if ((double)required * log2((double)cases[c].usable) >= (double)min_bits) {
            return false;
        }
        const double bias = seedtool_dice_entropy_bias_bits(cases[c].source);
        size_t flagged = 0;
        for (size_t t = 0; t < trials; ++t) {
            uint8_t values[SEEDTOOL_MAX_EVENTS];
            for (size_t i = 0; i < required; ++i) {
                values[i] = (uint8_t)(xorshift32(&state) % cases[c].usable) + 1;
            }
            int bits = 0;
            if (seedtool_dice_entropy_bits(cases[c].source, values, required, &bits) != SEEDTOOL_OK) {
                return false;
            }
            if (bits + (int)lround(bias) + dice_entropy_tolerance < min_bits) {
                ++flagged;
            }
        }
        if ((double)flagged / (double)trials < min_detection_rate) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    if (wally_init(0) != WALLY_OK) {
        return 1;
    }
    static const test_t tests[] = {
        TEST(dice_quality_is_sound),
        TEST(card_quality_is_sound),
        TEST(dice_entropy_false_positive_rate_is_bounded),
        TEST(honest_runs_report_at_least_the_minimum),
        TEST(insufficient_runs_are_flagged),
    };
    const int ret = run_tests("entropy-stats", tests, sizeof(tests) / sizeof(tests[0]));
    wally_cleanup(0);
    return ret;
}
