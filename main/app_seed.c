/* Where a mnemonic comes from: dice rolled, coins flipped or SeedPicker tickets
 * drawn straight into words, a checksum completed, or a backup restored. */
#include "app_internal.h"

#include <stdio.h>
#include <string.h>

/* 2048 words: eleven flips name one, every word equally likely. Most
 * significant first, the order seedtool_complete_checksum packs them in, so the
 * screen shows the arithmetic the seed is built from. */
#define COIN_WORD_BITS SEEDTOOL_WORD_BITS

/* Eleven picked words are 121 bits and twenty-three are 253; the entropy needs
 * 128 and 256. The remainder is flipped loose at the end, and BIP39's checksum
 * supplies the last 4 or 8 bits of the final word. */
static size_t picked_word_count(const size_t words) { return words == 12 ? 11 : 23; }
static size_t loose_bit_count(const size_t words) { return words == 12 ? 7 : 3; }

/* Flips bits[from..count) one coin at a time, the flips so far shown under
 * `title`. `tally`, when not NULL, is the run's bits before bits[0], and each
 * flip adds one. Backing out of a flip undoes the one before it; returns false
 * when the reader backs out of the first. */
static bool flip_bits(
    const char* title, uint8_t* bits, const size_t count, const size_t from, const seedtool_tally_t* tally)
{
    size_t i = from;
    while (i < count) {
        /* Rebuilt per flip, so only what is on screen is held. */
        char history[COIN_WORD_BITS + 1] = { 0 };
        for (size_t f = 0; f < i; ++f) {
            history[f] = (char)('0' + bits[f]);
        }
        unsigned bit = 0;
        const seedtool_tally_t so_far = tally ? (seedtool_tally_t){ tally->done + (unsigned)i, tally->total }
                                              : (seedtool_tally_t){ 0 };
        const bool flipped
            = enter_coin_flip(title, (unsigned)(i + 1), (unsigned)count, &bit, history, tally ? &so_far : NULL);
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

/* What a word was made from (`made`), the one-based number it names and the
 * word, checkable against a printed table. Going back discards that entry, so
 * it is asked rather than done: `verb` names redoing it ("Flip", "Roll",
 * "Draw") and `lost` what is thrown away. Returns true to accept, false to
 * enter the word again. */
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
        if (nav_screen(title, made, named, "Use this word", false) == NAV_CONFIRM) {
            accepted = true;
            break;
        }
        /* The question names the word it would discard. */
        char question[48], subject[32], again[16];
        (void)snprintf(question, sizeof(question), "%s word %u again?", verb, position);
        (void)snprintf(subject, sizeof(subject), "%s  %s", number, seedtool_word(index));
        (void)snprintf(again, sizeof(again), "%s again", verb);
        /* Opens on the arrow: the bar throws the entry away. */
        const int answer = nav_screen(question, subject, lost, again, true);
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

/* A source that makes a mnemonic a word at a time, each word eleven bits by
 * construction and checkable against a printed table. `run` is the source's
 * own state for one run - a die's rolls - or NULL. `intro` opens the run and
 * returns whether the reader started it; `enter` makes word `position`, the
 * run's bits before it in `tally`, and returns false when the reader backs out
 * of its first step; `made` writes what made a word, for its confirmation and,
 * `compact`, for a row of the review of the whole run. */
typedef struct {
    bool (*intro)(size_t words);
    bool (*enter)(void* run, unsigned position, unsigned total, const seedtool_tally_t* tally, uint16_t* index_out);
    void (*made)(const void* run, unsigned position, uint16_t index, bool compact, char* out, size_t out_len);
    /* Redoing a word, and what that throws away, for confirm_word. */
    const char* verb;
    const char* lost;
    const char* review_title;
    /* The dice keep their rolls in a dice_run_t for this die. */
    bool rolled;
    seedtool_die_t die;
} word_source_t;

/* The intro every source opens on: what to do, in two lines, and Start. */
static bool intro_screen(const char* title, const char* one, const char* two)
{
    return nav_screen(title, one, two, "Start", false) == NAV_CONFIRM;
}

/* Coins: eleven flips name one word, heads 1 and tails 0, most significant
 * first, the order seedtool_complete_checksum packs them in. */
static bool coin_intro(const size_t words)
{
    char needed[24];
    (void)snprintf(needed, sizeof(needed), "%u flips needed", words == 12 ? 128u : 256u);
    return intro_screen("Coin flips", needed, "Heads 1, tails 0");
}

/* One word's worth of flips. Backing out of a later flip undoes that flip
 * only. */
static bool enter_flipped_word(
    void* run, const unsigned position, const unsigned total, const seedtool_tally_t* tally, uint16_t* index_out)
{
    (void)run;
    uint8_t bits[COIN_WORD_BITS] = { 0 };
    /* The flip counter follows, so the two counts are told apart by name:
     * "Word 3/11, flip 5/11". Sized for ten-digit counts, for
     * -Wformat-truncation. */
    char title[48];
    (void)snprintf(title, sizeof(title), "Word %u/%u, flip", position, total);
    const bool flipped_all = flip_bits(title, bits, COIN_WORD_BITS, 0, tally);
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

/* The eleven flips behind a word, read back from its index. */
static void flips_made(const void* run, const unsigned position, const uint16_t index, const bool compact, char* out,
    const size_t out_len)
{
    (void)run;
    (void)position;
    (void)compact;
    size_t b = 0;
    for (; b < COIN_WORD_BITS && b + 1 < out_len; ++b) {
        out[b] = (char)('0' + ((index >> (COIN_WORD_BITS - 1 - b)) & 1u));
    }
    out[b] = '\0';
}

/* Dice: the faces that count give bits through seedtool_die_word; the rest
 * are rolled again before anything is entered, so the carousel offers only
 * the faces that count. The rolls are kept for the confirmation and the review:
 * a word's last roll keeps only its first bits, so two faces name the same
 * word and the word alone cannot say which was rolled. */
typedef struct {
    seedtool_die_t die;
    uint8_t rolls[23][SEEDTOOL_DIE_MAX_ROLLS];
} dice_run_t;

static bool d6_intro(const size_t words)
{
    (void)words;
    return intro_screen("D6 dice", "Only 1 to 4 count", "Roll 5 or 6 again");
}

static bool d20_intro(const size_t words)
{
    (void)words;
    return intro_screen("D20 dice", "Only 1 to 16 count", "Roll 17 to 20 again");
}

/* One word's rolls, one at a time, each adding its bits to the tally. Backing
 * out of a later roll returns to the roll before it. */
static bool enter_rolled_word(
    void* run, const unsigned position, const unsigned total, const seedtool_tally_t* tally, uint16_t* index_out)
{
    dice_run_t* const dice = run;
    uint8_t* const rolls = dice->rolls[position - 1];
    const size_t count = seedtool_die_rolls_per_word(dice->die);
    /* "Word 3/11, roll 2/6", told apart by name as the coins' are. */
    char title[48];
    (void)snprintf(title, sizeof(title), "Word %u/%u, roll", position, total);
    size_t r = 0;
    while (r < count) {
        unsigned face = 1;
        /* Only the last roll keeps fewer bits, and it adds them as the word
         * ends, so every roll shown is preceded by whole ones. */
        const seedtool_tally_t so_far = { tally->done + (unsigned)r * seedtool_die_bits(dice->die), tally->total };
        if (!enter_value(
                title, (unsigned)(r + 1), (unsigned)count, 1, seedtool_die_faces(dice->die), &face, &so_far)) {
            if (!r) {
                return false;
            }
            --r;
            continue;
        }
        rolls[r++] = (uint8_t)face;
    }
    seedtool_require(seedtool_die_word(dice->die, rolls, count, index_out) == SEEDTOOL_OK);
    return true;
}

/* A word's rolls, separated by commas so a D20's two-digit faces stay apart:
 * "4, 2, 1, 3, 3, 2", or "4,2,1,3,3,2" in a review row, which six spaced D6
 * rolls and the widest word overrun. */
static void rolls_made(const void* run, const unsigned position, const uint16_t index, const bool compact, char* out,
    const size_t out_len)
{
    (void)index;
    const dice_run_t* const dice = run;
    size_t used = 0;
    out[0] = '\0';
    for (size_t r = 0; r < seedtool_die_rolls_per_word(dice->die) && used < out_len; ++r) {
        const int written = snprintf(
            out + used, out_len - used, "%s%u", !r ? "" : compact ? "," : ", ", (unsigned)dice->rolls[position - 1][r]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
}

/* SeedPicker's tickets: says how many draws, and the one rule the device
 * cannot see kept: the ticket goes back in the bag every time. */
static bool ticket_intro(const size_t words)
{
    char needed[24];
    (void)snprintf(needed, sizeof(needed), "%u draws needed", (unsigned)picked_word_count(words));
    return intro_screen("SeedPicker tickets", needed, "Put each ticket back");
}

/* One word from SeedPicker's tickets: the ticket on the keypad, then the die
 * on its carousel. A pair past 2048 is drawn again from the ticket, not just
 * rerolled: rerolling the die alone would make T342's two words three times as
 * likely as any other. Neither the ticket nor the die alone is a whole number
 * of bits, so the tally moves only as the pair makes a word. Back from the die
 * returns to the ticket; returns false when the reader backs out of the
 * ticket. */
static bool enter_ticket_word(
    void* run, const unsigned position, const unsigned total, const seedtool_tally_t* tally, uint16_t* index_out)
{
    (void)run;
    char title[48];
    (void)snprintf(title, sizeof(title), "Ticket, word %u/%u", position, total);
    for (;;) {
        uint32_t ticket = 0;
        /* The keypad offers only digits that still lead to T1..T342. */
        if (!enter_number(title, TICKET_DIGITS, 1, SEEDTOOL_TICKETS, &ticket, tally)) {
            return false;
        }
        /* The ticket stays in view while the die is set. */
        char die_title[32];
        (void)snprintf(die_title, sizeof(die_title), "T%u die, word", (unsigned)ticket);
        unsigned die = 1;
        bool rolled = enter_value(die_title, position, total, 1, SEEDTOOL_TICKET_DIE, &die, tally);
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
 * exactly one pair. "T57 + die 4", or "T57+4" in a review row, the notation of
 * SeedPicker's form, which "T195 + die 3 mushroom" overruns. */
static void ticket_made(const void* run, const unsigned position, const uint16_t index, const bool compact, char* out,
    const size_t out_len)
{
    (void)run;
    (void)position;
    unsigned ticket = 0, die = 0;
    seedtool_require(seedtool_word_ticket(index, &ticket, &die) == SEEDTOOL_OK);
    (void)snprintf(out, out_len, compact ? "T%u+%u" : "T%u + die %u", ticket, die);
    seedtool_zero(&ticket, sizeof(ticket));
    seedtool_zero(&die, sizeof(die));
}

/* Every word of a run with what made it, four to a page, to check against the
 * paper before the loose bits are flipped. Returns true on the tick, false on
 * the arrow. */
static bool review_words(
    const word_source_t* source, const void* run, const uint16_t* indices, const size_t count)
{
    const size_t pages = (count + 3) / 4;
    size_t cursor = 1;
    int answer = NAV_STAY;
    char made[32] = { 0 };
    char rows[4][48] = { { 0 } };
    while (answer == NAV_STAY) {
        memset(rows, 0, sizeof(rows));
        const size_t page = page_shown(cursor, pages);
        for (size_t i = 0; i < 4 && page * 4 + i < count; ++i) {
            const size_t w = page * 4 + i;
            source->made(run, (unsigned)(w + 1), indices[w], true, made, sizeof(made));
            (void)snprintf(rows[i], sizeof(rows[i]), "%02u. %s %s", (unsigned)(w + 1), made, seedtool_word(indices[w]));
        }
        seedtool_zero(made, sizeof(made));
        char footer[24];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
        const seedtool_nav_t nav = page_nav(cursor, pages, footer, true, SEEDTOOL_CONFIRM_TICK);
        const char* const shown[] = { rows[0], rows[1], rows[2], rows[3] };
        seedtool_render_nav_rows(&nav, source->review_title, shown, 4);
        seedtool_display_flush();
        answer = page_key(&cursor, pages, true);
    }
    seedtool_zero(rows, sizeof(rows));
    return answer == NAV_CONFIRM;
}

/* Words made one at a time and packed straight into the entropy - eleven
 * flips, a die's rolls, or a ticket and a die per word - then reviewed whole,
 * then 7 or 3 loose coin bits, and BIP39's checksum finishes the last word,
 * through the same seedtool_complete_checksum complete_checksum uses. No hash
 * and no quality gate: every word is eleven bits by construction. Returns false
 * only when the reader backed out before any entry. */
static bool pick_words(const size_t words, const word_source_t* source, void* run)
{
    const size_t word_count = picked_word_count(words);
    const size_t tail_count = loose_bit_count(words);
    /* 128 or 256: the words' eleven bits each and the loose ones. */
    const unsigned entropy_bits = (unsigned)(word_count * SEEDTOOL_WORD_BITS + tail_count);
    uint16_t indices[23] = { 0 };
    char picked[23][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
    uint8_t tail[7] = { 0 };
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    char made[32] = { 0 };
    bool started = true;
    size_t w = 0;

    /* Set when the run steps back onto a finished word, which then reopens at
     * its confirmation rather than losing its entry. */
    bool reopen = false;

    for (;;) {
        while (w < word_count) {
            uint16_t index = indices[w];
            const seedtool_tally_t tally = { (unsigned)w * SEEDTOOL_WORD_BITS, entropy_bits };
            if (reopen) {
                reopen = false;
            } else if (!source->enter(run, (unsigned)(w + 1), (unsigned)word_count, &tally, &index)) {
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
            source->made(run, (unsigned)(w + 1), index, false, made, sizeof(made));
            const bool accepted
                = confirm_word((unsigned)(w + 1), (unsigned)word_count, index, made, source->verb, source->lost);
            seedtool_zero(made, sizeof(made));
            /* Declined at the confirmation: enter this same word again. */
            if (!accepted) {
                continue;
            }
            indices[w++] = index;
        }
        if (!started) {
            break;
        }

        /* The whole run; backing out of it returns to the last word, reopened
         * at its confirmation like every step back. */
        if (!review_words(source, run, indices, word_count)) {
            w = word_count - 1;
            reopen = true;
            continue;
        }

        /* The loose bits; backing out of the first returns to the review. */
        const seedtool_tally_t tally = { (unsigned)word_count * SEEDTOOL_WORD_BITS, entropy_bits };
        if (!flip_bits("Final bits", tail, tail_count, 0, &tally)) {
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

/* The sources, in the order the menu lists them. */
static const char* const SOURCE_NAMES[] = { "D6 dice", "D20 dice", "Coin flips", "SeedPicker tickets" };
static const word_source_t SOURCES[] = {
    { d6_intro, enter_rolled_word, rolls_made, "Roll", "These rolls are lost", "Check the rolls", true, SEEDTOOL_D6 },
    { d20_intro, enter_rolled_word, rolls_made, "Roll", "These rolls are lost", "Check the rolls", true,
        SEEDTOOL_D20 },
    { coin_intro, enter_flipped_word, flips_made, "Flip", "These flips are lost", "Check the flips", false,
        SEEDTOOL_D6 },
    { ticket_intro, enter_ticket_word, ticket_made, "Draw", "This draw is lost", "Check the draws", false,
        SEEDTOOL_D6 },
};
_Static_assert(ARRAY_LEN(SOURCE_NAMES) == ARRAY_LEN(SOURCES), "every source has a menu row");

/* Returns whether a seed was generated, rather than the reader backing out of a
 * picker. Leaving the wallet reboots, so true only comes back when the wallet
 * failed to open. */
static bool create_seed(void)
{
    size_t cursor = 0;
    for (;;) {
        const int source = choose_menu_at("Entropy source", SOURCE_NAMES, ARRAY_LEN(SOURCE_NAMES), &cursor);
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
            dice_run_t dice = { .die = SOURCES[source].die };
            void* const run = SOURCES[source].rolled ? &dice : NULL;
            bool generated = false;
            while (!generated && SOURCES[source].intro(words)) {
                /* Backing out of the first entry returns to the intro. */
                generated = pick_words(words, &SOURCES[source], run);
            }
            seedtool_zero(&dice, sizeof(dice));
            if (generated) {
                return true;
            }
            /* Back off the intro: the length picker, one stage. */
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
            if (!flip_bits("Coin flip", bits, bits_count, from, NULL)) {
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
