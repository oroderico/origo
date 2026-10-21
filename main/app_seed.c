/* Where a mnemonic comes from: entropy collected and hashed, coins flipped or
 * SeedPicker tickets drawn straight into words, a checksum completed, or a
 * backup restored. */
#include "app_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Returns false only when the reader steps back off the very first screen -
 * the caller still holds the entropy that produced this, and throwing it away
 * to start the run over is not what one press should mean. */
static bool show_generated(seedtool_generated_t* generated)
{
    char hash[65];
    hexstr(generated->hash, sizeof(generated->hash), hash);
    enum { STAGE_TRANSCRIPT, STAGE_HASH, STAGE_REVIEW, STAGE_DONE };
    int stage = STAGE_TRANSCRIPT;
    bool stepped_back = false;
    while (stage != STAGE_DONE) {
        switch (stage) {
        case STAGE_TRANSCRIPT:
            if (!page_text("Canonical transcript", generated->transcript)) {
                stepped_back = true;
                stage = STAGE_DONE;
                break;
            }
            stage = STAGE_HASH;
            break;
        case STAGE_HASH:
            stage = page_text("SHA256", hash) ? STAGE_REVIEW : STAGE_TRANSCRIPT;
            break;
        case STAGE_REVIEW:
            stage = review_backup_and_show_wallet(generated->mnemonic, generated->words) ? STAGE_DONE : STAGE_HASH;
            break;
        default:
            stage = STAGE_DONE;
            break;
        }
    }
    seedtool_zero(hash, sizeof(hash));
    return !stepped_back;
}

/* Bits so far and whether the run looks patterned, for every source, so the
 * live bar and the end-of-run gate agree. Cards without replacement are exact;
 * the rest go through the Shannon estimator on a 1-indexed copy of the values. */
static void entropy_quality(
    const seedtool_source_t source, const uint8_t* values, const size_t count, int* bits_out, bool* pattern_out)
{
    if (source == SEEDTOOL_CARDS) {
        (void)seedtool_card_entropy_bits(count, bits_out);
        (void)seedtool_card_pattern_detected(values, count, pattern_out);
        return;
    }
    /* Zeroed: only faces[0..count) is filled. */
    uint8_t faces[SEEDTOOL_MAX_EVENTS] = { 0 };
    const uint8_t* faces_ptr = values;
    if (source == SEEDTOOL_COIN || source == SEEDTOOL_CARDS_REPLACE) {
        for (size_t i = 0; i < count; ++i) {
            faces[i] = (uint8_t)(values[i] + 1);
        }
        faces_ptr = faces;
    }
    (void)seedtool_dice_entropy_bits(source, faces_ptr, count, bits_out);
    (void)seedtool_dice_pattern_detected(source, faces_ptr, count, pattern_out);
    /* Skipped at zero draws: there is no distribution yet to be biased. */
    if (count) {
        *bits_out += (int)lround(seedtool_dice_entropy_bias_bits(source));
    }
    /* A copy of the entropy, made on every keystroke. */
    seedtool_zero(faces, sizeof(faces));
}

/* Live quality readout: draws and bits so far against what the seed needs,
 * adapted from Krux (src/krux/pages/new_mnemonic/dice_rolls.py). Display only;
 * nothing here is hashed. */
static seedtool_progress_t entropy_progress(const seedtool_source_t source, const uint8_t* values, const size_t count,
    const size_t required, const size_t min_bits)
{
    int bits = 0;
    bool pattern = false;
    entropy_quality(source, values, count, &bits, &pattern);
    const int capped_bits = (size_t)bits < min_bits ? bits : (int)min_bits;
    const seedtool_progress_t progress = {
        .rolls_pct = (int)(count * 100 / required),
        .entropy_pct = (int)((size_t)capped_bits * 100 / min_bits),
        .warn = pattern,
        .complete = false, /* only called for count < required, so never yet */
        .graded = true,
        .bits = bits,
    };
    return progress;
}

/* Collects the whole transcript and generates from it. Returns false only when
 * the reader backed out before the first entry. */
static bool collect_entropy(const int source, const size_t words)
{
    /* "Coins", not "Coin flips": the longest name with the longest counter did
     * not fit the title. */
    static const char* const names[] = { "D6 dice", "D20 dice", "Coins", "Cards", "Cards" };
    static const char* const nouns[] = { "rolls", "rolls", "flips", "cards", "cards" };
    const size_t required = seedtool_required_events((seedtool_source_t)source, words);
    const unsigned max = source == SEEDTOOL_D6 ? 6 : 20;
    const size_t min_bits = seedtool_min_entropy_bits(words);

    uint8_t values[SEEDTOOL_MAX_EVENTS] = { 0 };
    bool available[52];
    char history[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1] = { 0 };
    seedtool_generated_t generated;
    bool started = true;
    size_t i = 0;
    memset(available, 1, sizeof(available));
    memset(&generated, 0, sizeof(generated));

    {
        /* Shown once, with the bar already in place but empty, so its shape and
         * position are seen before they start moving. */
        char needed[24];
        (void)snprintf(needed, sizeof(needed), "%u %s needed", (unsigned)required, nouns[source]);
        /* One deck without replacement cannot reach 256 bits, so 24-word cards
         * say how they are drawn. One line, since two wrapped into the bar. */
        const char* const hint
            = source == SEEDTOOL_CARDS_REPLACE ? "Return & reshuffle each card" : "Bar turns red on a pattern";
        const seedtool_progress_t empty = { 0 };
        started = nav_dice_confirm(names[source], needed, hint, "Start", false, &empty);
    }

    while (started) {
        while (i < required) {
            unsigned value = 0;
            /* Rebuilt from the values each time, so a step back unwrites the
             * last entry too. */
            if (seedtool_transcript((seedtool_source_t)source, values, i, history, sizeof(history)) != SEEDTOOL_OK) {
                history[0] = '\0';
            }
            /* The entries only: a card transcript's prefix names its encoding
             * for the hash, and on screen it reads as something entered. */
            const size_t prefix_len = strlen(SEEDTOOL_CARDS_TRANSCRIPT_PREFIX);
            const char* const entries
                = strncmp(history, SEEDTOOL_CARDS_TRANSCRIPT_PREFIX, prefix_len) == 0 ? history + prefix_len : history;
            const size_t shown = strlen(entries) - seedtool_render_fit_tail(entries);
            const seedtool_progress_t progress
                = entropy_progress((seedtool_source_t)source, values, i, required, min_bits);
            const char* const label = names[source];
            bool entered;
            if (source == SEEDTOOL_COIN) {
                entered = enter_coin_flip(label, (unsigned)(i + 1), (unsigned)required, &value, entries + shown,
                    &progress);
            } else if (source == SEEDTOOL_CARDS || source == SEEDTOOL_CARDS_REPLACE) {
                /* With replacement nothing is excluded. */
                entered = enter_card((unsigned)(i + 1), (unsigned)required, &value,
                    source == SEEDTOOL_CARDS ? available : NULL, entries + shown, &progress);
            } else {
                entered = enter_value(label, (unsigned)(i + 1), (unsigned)required, 1, max, &value, NULL, NULL,
                    entries + shown, &progress);
            }
            if (!entered) {
                if (!i) {
                    started = false;
                    break;
                }
                --i;
                /* A card put back becomes drawable again, or correcting an entry
                 * would leave it permanently out of the deck. */
                if (source == SEEDTOOL_CARDS) {
                    available[values[i]] = true;
                }
                continue;
            }
            values[i] = (uint8_t)value;
            if (source == SEEDTOOL_CARDS) {
                available[value] = false;
            }
            ++i;
        }
        if (!started) {
            break;
        }
        /* The run is fixed-length: declining steps back into the entry at the
         * last value. */
        int bits = 0;
        bool pattern = false;
        entropy_quality((seedtool_source_t)source, values, required, &bits, &pattern);
        const bool poor = (size_t)bits + DICE_ENTROPY_TOLERANCE < min_bits;
        bool proceed = true;
        /* Both verdicts show the number the decision was made on. */
        char bits_line[24];
        (void)snprintf(bits_line, sizeof(bits_line), "%d of %u bits", bits, (unsigned)min_bits);
        /* The verdict goes in the body, not the title: it is a finding about
         * the run, and belongs where the eye already is. */
        if (poor) {
            proceed = nav_acknowledge(NULL, "Poor entropy!", bits_line, "Proceed anyway", true);
        }
        if (proceed && pattern) {
            proceed = nav_acknowledge(NULL, "Pattern detected!", NULL, "Proceed anyway", true);
        }
        if (proceed && !poor && !pattern) {
            /* The one place `complete` is true (a green outline); the verdict
             * is also in words, for readers who cannot tell the colours apart. */
            const seedtool_progress_t complete = { .rolls_pct = 100, .entropy_pct = 100, .warn = false, .complete = true };
            proceed = nav_dice_confirm(names[source], bits_line, "Looks good", "Generate seed", false, &complete);
        }
        if (proceed) {
            if (seedtool_generate((seedtool_source_t)source, words, values, required, &generated) != SEEDTOOL_OK) {
                notice("Error", "Could not generate seed", NULL);
                break;
            }
            if (show_generated(&generated)) {
                break;
            }
            /* Back off the first screen after generating: the verdict again,
             * with every roll still here. */
            continue;
        }
        --i;
        /* Put the last card back, as the in-run back-step does, or it stays
         * struck off the deck. */
        if (source == SEEDTOOL_CARDS) {
            available[values[i]] = true;
        }
    }
    seedtool_zero(values, sizeof(values));
    seedtool_zero(available, sizeof(available));
    seedtool_zero(history, sizeof(history));
    seedtool_zero(&generated, sizeof(generated));
    return started;
}

/* 2048 words: eleven flips name one, every word equally likely. Most
 * significant first, the order seedtool_complete_checksum packs them in, so the
 * screen shows the arithmetic the seed is built from. */
#define COIN_WORD_BITS 11

/* Eleven picked words are 121 bits and twenty-three are 253; the entropy needs
 * 128 and 256. The remainder is flipped loose at the end, and BIP39's checksum
 * supplies the last 4 or 8 bits of the final word. */
static size_t picked_word_count(const size_t words) { return words == 12 ? 11 : 23; }
static size_t loose_bit_count(const size_t words) { return words == 12 ? 7 : 3; }

/* Flips bits[from..count) one coin at a time, the flips so far shown under
 * `title`. Backing out of a flip undoes the one before it; returns false when
 * the reader backs out of the first. */
static bool flip_bits(const char* title, uint8_t* bits, const size_t count, const size_t from)
{
    size_t i = from;
    while (i < count) {
        /* Rebuilt per flip, so only what is on screen is held. */
        char history[COIN_WORD_BITS + 1] = { 0 };
        for (size_t f = 0; f < i; ++f) {
            history[f] = (char)('0' + bits[f]);
        }
        unsigned bit = 0;
        const bool flipped = enter_coin_flip(title, (unsigned)(i + 1), (unsigned)count, &bit, history, NULL);
        seedtool_zero(history, sizeof(history));
        if (!flipped) {
            if (!i) {
                return false;
            }
            --i;
            continue;
        }
        bits[i++] = (uint8_t)bit;
    }
    return true;
}

/* One word's worth of flips. Returns true with `*index_out` set, or false when
 * the reader backed out of its first flip. Backing out of a later flip undoes
 * that flip only. */
static bool enter_flipped_word(const unsigned position, const unsigned total, uint16_t* index_out)
{
    uint8_t bits[COIN_WORD_BITS] = { 0 };
    /* The flip counter follows, so the two counts are told apart by name:
     * "Word 3/11, flip 5/11". Sized for ten-digit counts, for
     * -Wformat-truncation. */
    char title[48];
    (void)snprintf(title, sizeof(title), "Word %u/%u, flip", position, total);
    const bool flipped_all = flip_bits(title, bits, COIN_WORD_BITS, 0);
    if (flipped_all) {
        uint16_t index = 0;
        for (size_t b = 0; b < COIN_WORD_BITS; ++b) {
            index = (uint16_t)((index << 1) | bits[b]);
        }
        *index_out = index;
    }
    seedtool_zero(bits, sizeof(bits));
    return flipped_all;
}

/* What a word was made from (`made`), the one-based number it names and the
 * word, checkable against a printed table. Going back discards that entry, so
 * it is asked rather than done: `verb` names redoing it ("Flip", "Draw") and
 * `lost` what is thrown away. Returns true to accept, false to enter the word
 * again. */
static bool confirm_word(const unsigned position, const unsigned total, const uint16_t index, const char* made,
    const char* verb, const char* lost)
{
    char title[48], number[16];
    (void)snprintf(title, sizeof(title), "Word %u/%u", position, total);
    (void)snprintf(number, sizeof(number), "%04u", (unsigned)index + 1u);
    char named[48];
    (void)snprintf(named, sizeof(named), "%s  %s", number, seedtool_word(index));
    bool accepted;
    for (;;) {
        /* Number and word on one line, so both body lines carry what is
         * checked. */
        if (nav_screen(title, made, named, "Use this word", false, NULL) == NAV_CONFIRM) {
            accepted = true;
            break;
        }
        /* The question names the word it would discard. */
        char question[48], subject[32], again[16];
        (void)snprintf(question, sizeof(question), "%s word %u again?", verb, position);
        (void)snprintf(subject, sizeof(subject), "%s  %s", number, seedtool_word(index));
        (void)snprintf(again, sizeof(again), "%s again", verb);
        /* Opens on the arrow: the bar throws the entry away. */
        const int answer = nav_screen(question, subject, lost, again, true, NULL);
        seedtool_zero(subject, sizeof(subject));
        if (answer == NAV_CONFIRM) {
            accepted = false;
            break;
        }
        /* Declined: back to the word, unchanged. */
    }
    seedtool_zero(named, sizeof(named));
    seedtool_zero(number, sizeof(number));
    return accepted;
}

/* The eleven flips behind a flipped word. */
static bool confirm_flipped_word(const unsigned position, const unsigned total, const uint16_t index)
{
    char bitline[COIN_WORD_BITS + 1] = { 0 };
    for (size_t b = 0; b < COIN_WORD_BITS; ++b) {
        bitline[b] = (char)('0' + ((index >> (COIN_WORD_BITS - 1 - b)) & 1u));
    }
    const bool accepted = confirm_word(position, total, index, bitline, "Flip", "These flips are lost");
    seedtool_zero(bitline, sizeof(bitline));
    return accepted;
}

/* One word from SeedPicker's tickets: the ticket on the keypad, then the die
 * on its carousel. A pair past 2048 is drawn again from the ticket, not just
 * rerolled: rerolling the die alone would make T342's two words three times as
 * likely as any other. Back from the die returns to the ticket; returns false
 * when the reader backs out of the ticket. */
static bool enter_ticket_word(const unsigned position, const unsigned total, uint16_t* index_out)
{
    char title[48];
    (void)snprintf(title, sizeof(title), "Ticket, word %u/%u", position, total);
    for (;;) {
        uint32_t ticket = 0;
        if (!enter_number(title, 3, &ticket)) {
            return false;
        }
        if (ticket < 1 || ticket > SEEDTOOL_TICKETS) {
            notice("No such ticket", "Tickets run", "T1 to T342");
            seedtool_zero(&ticket, sizeof(ticket));
            continue;
        }
        /* The ticket stays in view while the die is set. */
        char die_title[32];
        (void)snprintf(die_title, sizeof(die_title), "T%u die, word", (unsigned)ticket);
        unsigned die = 1;
        bool rolled = enter_value(die_title, position, total, 1, SEEDTOOL_TICKET_DIE, &die, NULL, NULL, NULL, NULL);
        seedtool_zero(die_title, sizeof(die_title));
        if (rolled && seedtool_ticket_word(ticket, die, index_out) != SEEDTOOL_OK) {
            char past[32];
            (void)snprintf(past, sizeof(past), "T%u + %u is past 2048", (unsigned)ticket, die);
            notice("Draw a new ticket", past, "Put this one back");
            seedtool_zero(past, sizeof(past));
            rolled = false;
        }
        seedtool_zero(&ticket, sizeof(ticket));
        seedtool_zero(&die, sizeof(die));
        if (rolled) {
            return true;
        }
    }
}

/* The ticket and die behind a word, recovered from its index: each word has
 * exactly one pair. */
static bool confirm_ticket_word(const unsigned position, const unsigned total, const uint16_t index)
{
    char drawn[32];
    (void)snprintf(drawn, sizeof(drawn), "T%u + die %u", (unsigned)index / SEEDTOOL_TICKET_DIE + 1u,
        (unsigned)index % SEEDTOOL_TICKET_DIE + 1u);
    const bool accepted = confirm_word(position, total, index, drawn, "Draw", "This draw is lost");
    seedtool_zero(drawn, sizeof(drawn));
    return accepted;
}

/* One word of a run made word by word: `enter` returns false when the reader
 * backs out of its first step, `confirm` false to enter the word again. */
typedef bool (*word_entry_fn)(unsigned position, unsigned total, uint16_t* index_out);
typedef bool (*word_confirm_fn)(unsigned position, unsigned total, uint16_t index);

/* Words chosen one at a time and packed straight into the entropy rather than
 * hashed - eleven coin flips, or a ticket and a die, per word - then 7 or 3
 * loose coin bits, and BIP39's checksum finishes the last word, through the same
 * seedtool_complete_checksum complete_checksum uses. No quality gate: every
 * word is eleven bits by construction. Returns false only when the reader
 * backed out before any entry. */
static bool pick_words(const size_t words, const word_entry_fn enter, const word_confirm_fn confirm)
{
    const size_t word_count = picked_word_count(words);
    const size_t tail_count = loose_bit_count(words);
    uint16_t indices[23] = { 0 };
    char picked[23][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
    uint8_t tail[7] = { 0 };
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    bool started = true;
    size_t w = 0;

    /* Set when the run steps back onto a finished word, which then reopens at
     * its confirmation rather than losing its entry. */
    bool reopen = false;

    for (;;) {
        while (w < word_count) {
            uint16_t index = indices[w];
            if (reopen) {
                reopen = false;
            } else if (!enter((unsigned)(w + 1), (unsigned)word_count, &index)) {
                /* Back from a word's first entry reopens the word before it;
                 * only the very first entry leaves the run. */
                if (!w) {
                    started = false;
                    break;
                }
                --w;
                reopen = true;
                continue;
            }
            /* Declined at the confirmation: enter this same word again. */
            if (!confirm((unsigned)(w + 1), (unsigned)word_count, index)) {
                continue;
            }
            indices[w++] = index;
        }
        if (!started) {
            break;
        }

        /* The loose bits; backing out of the first returns to the last word,
         * reopened at its confirmation like every step back. */
        if (!flip_bits("Final bits", tail, tail_count, 0)) {
            w = word_count - 1;
            reopen = true;
            continue;
        }

        for (size_t i = 0; i < word_count; ++i) {
            strcpy(picked[i], seedtool_word(indices[i]));
        }
        /* Checked, not assumed: a truncated prefix would be a wrong seed. */
        seedtool_require(join_words(picked, word_count, prefix, sizeof(prefix)));

        if (seedtool_complete_checksum(prefix, tail, tail_count, completed, sizeof(completed)) != SEEDTOOL_OK) {
            notice("Error", "Could not complete", "the mnemonic");
            break;
        }
        review_backup_and_show_wallet(completed, words);
        break;
    }

    seedtool_zero(indices, sizeof(indices));
    seedtool_zero(picked, sizeof(picked));
    seedtool_zero(tail, sizeof(tail));
    seedtool_zero(prefix, sizeof(prefix));
    seedtool_zero(completed, sizeof(completed));
    return started;
}

/* Flipped words or hashed flips. Only coins get this choice: theirs is the one
 * transcript a reader can turn into words by hand. Returns 0 for flipped words,
 * 1 for hashed, NAV_BACK for back. */
static int choose_coin_method(void)
{
    const char* methods[] = { "Flip each word", "Flip and hash" };
    return choose_menu("Coin method", methods, ARRAY_LEN(methods));
}

/* The "Entropy source" menu's rows are the sources, in enum order. */
_Static_assert(SEEDTOOL_D6 == 0 && SEEDTOOL_D20 == 1 && SEEDTOOL_COIN == 2 && SEEDTOOL_CARDS == 3,
    "the entropy source menu is indexed by seedtool_source_t");
/* The row after them: SeedPicker's tickets, which build words directly and so
 * have no transcript and no seedtool_source_t of their own. */
#define SOURCE_ROW_TICKETS 4

/* Returns whether a seed was generated, rather than the reader backing out of a
 * picker. Leaving the wallet reboots, so true only comes back when the wallet
 * failed to open. */
static bool create_seed(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* sources[] = { "D6 dice", "D20 dice", "Coin flips", "Cards", "SeedPicker tickets" };
        const int source = choose_menu_at("Entropy source", sources, ARRAY_LEN(sources), &cursor);
        if (source == NAV_BACK) {
            return false;
        }
        for (;;) {
            const char* lengths[] = { "12 words", "24 words" };
            const int length = choose_menu("Seed length", lengths, ARRAY_LEN(lengths));
            if (length == NAV_BACK) {
                break;
            }
            const size_t words = length ? 24 : 12;
            if (source == SOURCE_ROW_TICKETS) {
                if (pick_words(words, enter_ticket_word, confirm_ticket_word)) {
                    return true;
                }
                continue; /* Backed out before any entry: the length picker. */
            }
            if (source == SEEDTOOL_COIN) {
                bool run_hashed = false;
                for (;;) {
                    const int method = choose_coin_method();
                    if (method == NAV_BACK) {
                        break; /* Back: the length picker, one stage. */
                    }
                    if (method == 1) {
                        run_hashed = true;
                        break;
                    }
                    if (pick_words(words, enter_flipped_word, confirm_flipped_word)) {
                        return true;
                    }
                    /* Backed out before any entry: this menu again. */
                }
                if (!run_hashed) {
                    continue;
                }
            }
            /* 24-word cards draw with replacement (see SEEDTOOL_CARDS_REPLACE). */
            const int collect_source = length && source == SEEDTOOL_CARDS ? SEEDTOOL_CARDS_REPLACE : source;
            if (collect_entropy(collect_source, words)) {
                return true;
            }
        }
    }
}

/* Same contract as create_seed(). */
static bool complete_checksum(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* lengths[] = { "11 words + 7 coins", "23 words + 3 coins" };
        const int selected = choose_menu_at("Complete checksum", lengths, ARRAY_LEN(lengths), &cursor);
        if (selected == NAV_BACK) {
            return false;
        }
        const size_t count = selected ? 23 : 11;
        const size_t bits_count = selected ? 3 : 7;
        char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        uint8_t bits[7] = { 0 };
        /* Held here so stepping back from the flips hands the words back, with
         * the method they were typed by. */
        char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
        bool by_number = false;
        bool entered = enter_mnemonic_words(count, words, &by_number);
        if (entered) {
            seedtool_require(join_words(words, count, prefix, sizeof(prefix)));
        }
        /* Flips and result in one loop, so backing off the result is the last
         * flip again. */
        size_t from = 0;
        while (entered) {
            if (!flip_bits("Coin flip", bits, bits_count, from)) {
                /* Back from the first flip returns to the list of words. */
                entered = revisit_prefix(words, count, &by_number);
                if (entered) {
                    seedtool_require(join_words(words, count, prefix, sizeof(prefix)));
                }
                from = 0;
                continue;
            }
            if (seedtool_complete_checksum(prefix, bits, bits_count, completed, sizeof(completed)) != SEEDTOOL_OK) {
                /* Said, not fallen through silently. */
                notice("Error", "Could not complete", "the checksum");
                break;
            }
            if (page_text("Completed mnemonic", completed)) {
                show_wallet_data(completed);
                break;
            }
            /* Back off the completed mnemonic: the last flip again, with the
             * words and every flip before it still in hand. */
            from = bits_count - 1;
        }
        seedtool_zero(words, sizeof(words));
        seedtool_zero(prefix, sizeof(prefix));
        seedtool_zero(completed, sizeof(completed));
        seedtool_zero(bits, sizeof(bits));
        if (entered) {
            return true;
        }
    }
}

/* A new seed from entropy, or finishing one whose last word is missing (from a
 * Stackbit backup, say). Both end in the wallet. */
void show_new_seed_menu(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* items[] = { "From entropy", "Complete checksum" };
        const int selected = choose_menu_at("New Seed", items, ARRAY_LEN(items), &cursor);
        if (selected == NAV_BACK) {
            return;
        }
        /* A generated seed returns home. Leaving the wallet reboots, so this
         * only runs when the wallet failed to open. */
        if (selected == 0 ? create_seed() : complete_checksum()) {
            return;
        }
    }
}

void restore_seed(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* lengths[] = { "12 words", "24 words" };
        const int selected = choose_menu_at("Restore mnemonic", lengths, ARRAY_LEN(lengths), &cursor);
        if (selected == NAV_BACK) {
            return;
        }
        const size_t count = selected ? 24 : 12;
        char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        /* Held here, like complete_checksum's, so that every step back from
         * here on hands the words over rather than asking for them again. */
        char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
        bool by_number = false;
        bool resume = false;
        bool restored;
        for (;;) {
            restored = restore_mnemonic(count, words, &by_number, mnemonic, sizeof(mnemonic), resume);
            resume = false;
            if (!restored) {
                break;
            }
            if (!nav_acknowledge(
                    "Checksum valid", "BIP39 English", "Derivation unlocked", "Open wallet", false)) {
                /* Back off the verdict is the review again, with a restore
                 * that already succeeded still in hand. */
                resume = true;
                continue;
            }
            show_wallet_data(mnemonic);
            break;
        }
        seedtool_zero(words, sizeof(words));
        seedtool_zero(mnemonic, sizeof(mnemonic));
        if (restored) {
            return;
        }
    }
}
