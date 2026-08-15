#include "seedtool_app.h"
#include "seedtool_bbqr.h"
#include "seedtool_core.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#include "qrcode.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wally_core.h>

/* Published BIP84/BIP86 vectors for the all-zero entropy mnemonic. */
static const char mnemonic[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
static const char expected_address[] = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
/* BIP84's own published change vector, m/84'/0'/0'/1/0 for the same mnemonic -
 * the spec publishes both branches, so the change side is pinned to a vector
 * this project did not invent either. */
static const char expected_change_address[] = "bc1q8c6fshw2dlwun7ekn9qwf37cu2rn755upcp6el";
static const char expected_xpub84[]
    = "xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuyvz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhLv1VMrjPC7PW6V";
static const char expected_xpub86[]
    = "xpub6BgBgsespWvERF3LHQu6CnqdvfEvtMcQjYrcRzx53QJjSxarj2afYWcLteoGVky7D3UKDP9QyrLprQ3VCECoY49yfdDEHGCtMMj92pReUsQ";
/* SLIP-132 zpub for the same account: the identical key, xpub's version bytes
 * swapped for zpub's, published in the SLIP-132 test vectors. */
static const char expected_zpub84[]
    = "zpub6rFR7y4Q2AijBEqTUquhVz398htDFrtymD9xYYfG1m4wAcvPhXNfE3EfH1r1ADqtfSdVCToUG868RvUUkgDKf31mGDtKsAYz2oz2AGutZYs";
/* Published BIP39 vector for the all-zero 256-bit entropy. */
static const char mnemonic24[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
      "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";
/* Same words as `mnemonic`, checksum deliberately broken. */
static const char bad_checksum_mnemonic[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon";

/* The word-entry keyboard is only usable if every reachable letter really does
 * lead to a word, and if narrowing always terminates in a listable candidate
 * set. Both are checked exhaustively rather than by example. */
static bool wordlist_completion_is_sound(void)
{
    uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
    bool enabled[SEEDTOOL_LETTERS];

    if (seedtool_words_with_prefix("", 0, candidates, SEEDTOOL_MAX_WORD_CHOICES) != SEEDTOOL_WORDLIST_LEN
        || seedtool_next_letters("", 0, enabled) == 0) {
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
            if (!seedtool_words_with_prefix(word, prefix_len, candidates, SEEDTOOL_MAX_WORD_CHOICES)
                || !seedtool_next_letters(word, prefix_len, enabled) || !enabled[word[prefix_len] - 'a']) {
                return false;
            }
        }
        /* Typing four letters always narrows to a set the carousel can show. */
        const size_t stem = length < 4 ? length : 4;
        if (seedtool_words_with_prefix(word, stem, candidates, SEEDTOOL_MAX_WORD_CHOICES)
            > SEEDTOOL_MAX_WORD_CHOICES) {
            return false;
        }
    }
    return true;
}

/* The panel takes each pixel most significant byte first; the framebuffer holds
 * it the other way. Nothing on the host exercises that conversion — origo_sdl.c
 * replaces the whole driver — so the arithmetic is pinned here, including the
 * two colours whose symmetry hid the bug for as long as the screens were text. */
static bool wire_order_is_big_endian(void)
{
    static uint16_t wire[SEEDTOOL_DISPLAY_WIDTH * 5];
    const char* const items[] = { "Create Seed", "Restore Seed", "Complete Checksum" };

    seedtool_render_list("Origo", items, 3, 0, 0, "1/3");
    const uint16_t* const pixels = seedtool_render_pixels();
    for (size_t row = 0; row + 5 <= SEEDTOOL_DISPLAY_HEIGHT; row += 5) {
        seedtool_render_wire_rows(wire, row, 5);
        for (size_t i = 0; i < SEEDTOOL_DISPLAY_WIDTH * 5; ++i) {
            const uint16_t pixel = pixels[row * SEEDTOOL_DISPLAY_WIDTH + i];
            if (wire[i] != (uint16_t)((pixel >> 8) | (pixel << 8))) {
                return false;
            }
            /* Black and white must survive unchanged: they are what made a
             * missing swap look like a working display. */
            if ((pixel == 0x0000 || pixel == 0xffff) && wire[i] != pixel) {
                return false;
            }
        }
    }
    /* The selection orange is the colour that actually came out blue. */
    seedtool_render_clear();
    seedtool_render_list("Origo", items, 3, 0, 0, "1/3");
    seedtool_render_wire_rows(wire, 21, 1);
    bool orange_found = false;
    for (size_t i = 0; i < SEEDTOOL_DISPLAY_WIDTH; ++i) {
        if (pixels[21 * SEEDTOOL_DISPLAY_WIDTH + i] == 0xfd20) {
            orange_found = true;
            if (wire[i] != 0x20fd) {
                return false;
            }
        }
    }
    return orange_found;
}

/* Rearranging a layout by hand is exactly where a character goes missing without
 * anyone noticing, so the layouts are checked rather than trusted: no row wider
 * than the renderer draws, and a centre that is a real key. */
static bool layout_is_well_formed(const char* layout)
{
    size_t row = 0;
    for (const char* cursor = layout; *cursor;) {
        const char* end = cursor;
        while (*end && *end != '\n') {
            ++end;
        }
        if ((size_t)(end - cursor) > 10 || end == cursor) {
            return false;
        }
        ++row;
        cursor = *end ? end + 1 : end;
    }
    return row && seedtool_layout_center(layout) < seedtool_layout_keys(layout);
}

static bool layouts_are_complete(void)
{
    bool seen[128] = { false };

    if (!layout_is_well_formed(SEEDTOOL_WORD_LAYOUT) || !layout_is_well_formed(SEEDTOOL_WORD_NUMBER_LAYOUT)
        || seedtool_layout_keys(SEEDTOOL_WORD_LAYOUT) != SEEDTOOL_LETTERS + 1
        || seedtool_layout_keys(SEEDTOOL_WORD_NUMBER_LAYOUT) != SEEDTOOL_DIGITS + 2) {
        return false;
    }
    /* Every letter exactly once, and exactly one backspace: a QWERTY row with a
     * letter dropped would make some BIP39 words impossible to type. */
    for (size_t i = 0; i < seedtool_layout_keys(SEEDTOOL_WORD_LAYOUT); ++i) {
        const unsigned char key = (unsigned char)seedtool_layout_key(SEEDTOOL_WORD_LAYOUT, i);
        if (key != SEEDTOOL_KEY_BACKSPACE && (key < 'a' || key > 'z')) {
            return false;
        }
        if (seen[key]) {
            return false;
        }
        seen[key] = true;
    }
    for (unsigned char letter = 'a'; letter <= 'z'; ++letter) {
        if (!seen[letter]) {
            return false;
        }
    }
    /* The number keyboard's characters, not just its key count. enter_word_number
     * indexes reachable[key - '0'] for every key that is neither backspace nor
     * accept (seedtool_app.c), so a non-digit smuggled into this layout reads
     * off the end of a ten-element stack array. The letter layout above has
     * always had its characters checked; this one had only its length. */
    memset(seen, 0, sizeof(seen));
    for (size_t i = 0; i < seedtool_layout_keys(SEEDTOOL_WORD_NUMBER_LAYOUT); ++i) {
        const unsigned char key = (unsigned char)seedtool_layout_key(SEEDTOOL_WORD_NUMBER_LAYOUT, i);
        if (key != SEEDTOOL_KEY_BACKSPACE && key != SEEDTOOL_KEY_ACCEPT && (key < '0' || key > '9')) {
            return false;
        }
        if (seen[key]) {
            return false;
        }
        seen[key] = true;
    }
    for (unsigned char digit = '0'; digit <= '9'; ++digit) {
        if (!seen[digit]) {
            return false;
        }
    }
    /* The passphrase pages together must still reach all 95 printable ASCII
     * characters, or a passphrase typed on an older build could not be retyped
     * on this one. Space repeats on every page on purpose. */
    memset(seen, 0, sizeof(seen));
    for (size_t page = 0; page < SEEDTOOL_PASSPHRASE_PAGES; ++page) {
        const char* const layout = seedtool_passphrase_layouts[page];
        if (!layout_is_well_formed(layout)) {
            return false;
        }
        for (size_t i = 0; i < seedtool_layout_keys(layout); ++i) {
            seen[(unsigned char)seedtool_layout_key(layout, i)] = true;
        }
    }
    for (unsigned char character = 0x20; character <= 0x7e; ++character) {
        if (!seen[character]) {
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
        if (!seedtool_next_digits(digits, typed, enabled) || !enabled[digits[typed] - '0']) {
            return false;
        }
        if (seedtool_word_number(digits, typed) != 0 && typed + 1 < digits_len
            && seedtool_word_number(digits, typed + 1) == 0) {
            /* A prefix that is itself a word number may only be extended into
             * another one, or accept would be offered for a number the user is
             * still in the middle of typing. */
            return false;
        }
    }
    return seedtool_word_number(digits, digits_len) == expected && seedtool_word(expected - 1) != NULL;
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
        if (seedtool_word_number(rejected[i], strlen(rejected[i])) != 0) {
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

/* Every label the choice list can show. A label wider than a row is cut at the
 * last glyph that fits; nothing here is transcribed, but a silently shortened
 * label still misnames what the buttons are about to do. */
static const char* const menu_labels[] = { "Master fingerprint", "Native SegWit (BIP84)", "Taproot (BIP86)",
    "Account key", "Addresses", "Account key format", "xpub", "zpub", "Done / erase", "New Seed", "From entropy",
    "Restore Seed", "Complete checksum", "About", "Settings", "11 words + 7 coins", "23 words + 3 coins",
    "No passphrase", "Enter passphrase", "D6 dice", "D20 dice", "Coin flips", "Cards", "Back", "12 words",
    "24 words", "[delete]", "[back]", "Type the letters", "Enter word numbers", "Plain text", "Backup",
    "Stackbit 1248", "Compact SeedQR", "Simple grid", "Physical layout" };
#define MENU_LABEL_COUNT (sizeof(menu_labels) / sizeof(menu_labels[0]))

static bool labels_fit_a_row(void)
{
    for (size_t i = 0; i < MENU_LABEL_COUNT; ++i) {
        if (seedtool_render_fit_row(menu_labels[i]) != strlen(menu_labels[i])) {
            return false;
        }
    }
    return true;
}

/* Scrolling is only navigation if the selection is always on screen and the end
 * of a list looks like the end of it. Both hold for every list the firmware can
 * build, from any previous scroll position, so both are checked exhaustively. */
static bool list_viewport_is_sound(void)
{
    const int track = 99;
    /* The largest list the firmware builds is the address browser's, at 50
     * shown addresses plus "Go to index" and Back. The sweep runs well past
     * that on purpose: it costs nothing here, and it used to be a hundred-row
     * list, so the headroom is what keeps this test from having to move every
     * time that screen is resized. */
    for (size_t count = 1; count <= 101; ++count) {
        for (size_t selected = 0; selected < count; ++selected) {
            for (size_t previous = 0; previous <= count; ++previous) {
                const size_t top = seedtool_list_top(count, selected, previous);
                if (top > selected || selected >= top + SEEDTOOL_LIST_ROWS) {
                    return false;
                }
                if (count <= SEEDTOOL_LIST_ROWS ? top != 0 : top + SEEDTOOL_LIST_ROWS > count) {
                    return false;
                }
                /* The thumb must stay inside its track, stay big enough to see,
                 * and touch each end exactly when the list is at that end. */
                const seedtool_thumb_t thumb = seedtool_list_thumb(count, top, track);
                if (thumb.offset < 0 || thumb.height <= 0 || thumb.offset + thumb.height > track) {
                    return false;
                }
                if (count > SEEDTOOL_LIST_ROWS) {
                    if (thumb.height < 10 || thumb.height >= track) {
                        return false;
                    }
                    if ((top == 0) != (thumb.offset == 0)) {
                        return false;
                    }
                    if ((top == count - SEEDTOOL_LIST_ROWS) != (thumb.offset + thumb.height == track)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/* The entry screen shows the transcript as it grows, to be compared against
 * paper while it is still cheap to fix. That is only honest if a partial
 * transcript is a prefix of the finished one — a separator that appeared or
 * changed later would mean the reader checked a string the device then rewrote.
 * Checked for every prefix length of every source. */
static bool partial_transcripts_are_prefixes(void)
{
    static const seedtool_source_t sources[]
        = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_COIN, SEEDTOOL_CARDS, SEEDTOOL_CARDS_REPLACE };
    static const uint8_t limits[] = { 6, 20, 1, 51, 51 };
    for (size_t s = 0; s < 5; ++s) {
        uint8_t values[52];
        const size_t count = sources[s] == SEEDTOOL_CARDS ? 25 : sources[s] == SEEDTOOL_CARDS_REPLACE ? 48 : 40;
        for (size_t i = 0; i < count; ++i) {
            /* Deliberately repeats (only 5 distinct values across 48 draws):
             * unlike SEEDTOOL_CARDS, a repeat here must be accepted, not
             * rejected as a data-integrity error. */
            if (sources[s] == SEEDTOOL_CARDS_REPLACE) {
                values[i] = (uint8_t)(i % 5);
            } else if (sources[s] == SEEDTOOL_CARDS) {
                values[i] = (uint8_t)(i * 2 + 1);
            } else {
                values[i] = (uint8_t)((sources[s] == SEEDTOOL_COIN ? 0 : 1) + (i * 7 + 3) % limits[s]);
            }
        }
        char whole[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1];
        if (seedtool_transcript(sources[s], values, count, whole, sizeof(whole)) != SEEDTOOL_OK) {
            return false;
        }
        for (size_t k = 0; k <= count; ++k) {
            char partial[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1];
            if (seedtool_transcript(sources[s], values, k, partial, sizeof(partial)) != SEEDTOOL_OK
                || strncmp(whole, partial, strlen(partial)) != 0) {
                return false;
            }
            /* And the tail actually shown must be a tail of it, never longer. */
            const size_t tail = seedtool_render_fit_tail(partial);
            if (tail > strlen(partial)) {
                return false;
            }
        }
    }
    return true;
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

/* Paging splits by pixel width, so the chunks must still reassemble byte for
 * byte: a dropped character in a displayed xpub or address would be transcribed
 * as fact. */
static bool paging_is_lossless(const char* text)
{
    char rebuilt[512] = { 0 };
    const size_t total = strlen(text);
    size_t offset = 0, used = 0;
    while (offset < total) {
        const size_t fit = seedtool_render_fit(text + offset, 48);
        if (!fit || used + fit >= sizeof(rebuilt)) {
            return false;
        }
        memcpy(rebuilt + used, text + offset, fit);
        used += fit;
        offset += fit;
    }
    rebuilt[used] = '\0';
    return strcmp(rebuilt, text) == 0;
}

/* Vectors for the live dice-roll quality readout, adapted from Krux's
 * dice-roll entropy screen (github.com/selfcustody/krux). A UI signal only:
 * these never touch what seedtool_generate hashes into the seed. */
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

    /* A perfect round-robin: every face equally often, which alone would look
     * like enough entropy, but entirely predictable, which only the
     * derivative check below catches. */
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

/* Vectors for the live card-draw quality readout. Bits are exact (see
 * seedtool_card_entropy_bits' doc comment), not estimated, so this checks
 * boundaries and monotonicity rather than restating the log2 formula the
 * function itself computes - a check on its loop/off-by-one behavior, not a
 * tautology. */
static bool card_quality_is_sound(void)
{
    int bits = -1;
    if (seedtool_card_entropy_bits(0, &bits) != SEEDTOOL_OK || bits != 0) {
        return false;
    }
    /* The required draw count for a 12-word seed (seedtool_required_events)
     * must clear the 128-bit minimum by construction - nothing here is
     * estimated, so there is no room for doubt the way a dice run has. */
    if (seedtool_card_entropy_bits(25, &bits) != SEEDTOOL_OK || bits < (int)seedtool_min_entropy_bits(12)) {
        return false;
    }
    /* Non-decreasing over the whole deck: every further card at least does
     * not remove information, with equality only ever possible at the very
     * last card, which is fully determined by the other 51 and so adds
     * exactly zero. */
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

    /* A fresh, unshuffled deck read off in order: rank climbs 0..12 within
     * each suit before resetting, the same "counting through the faces"
     * shape seedtool_dice_pattern_detected catches for dice. */
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

/* seedtool_dice_entropy_bits is a small-sample-biased estimator (see
 * seedtool_dice_entropy_bias_bits' doc comment); the "poor entropy" gate in
 * collect_entropy (main/seedtool_app.c) corrects for that bias before
 * comparing against seedtool_min_entropy_bits. This proves the correction
 * actually keeps genuinely random rolls, at the exact roll counts
 * seedtool_required_events hands out, from being flagged "poor" more than an
 * acceptable fraction of the time -- the false positive this whole mechanism
 * exists to fix (D20 was previously flagged on essentially every random
 * run). DICE_ENTROPY_TOLERANCE is duplicated here rather than exposed from
 * seedtool_app.c, since it is UI-gate policy, not core behaviour. */
static bool dice_entropy_false_positive_rate_is_bounded(void)
{
    /* Coin included at its actual required count (128/256, exactly the
     * theoretical minimum - no cushion past it the way dice's roll counts
     * carry, a known, accepted trade-off): this is the empirical check that
     * its false-positive rate still lands under the same bound, not just
     * the back-of-envelope estimate that motivated accepting it. Same for
     * SEEDTOOL_CARDS_REPLACE at whatever count it currently asks for, read
     * from seedtool_required_events below rather than named here (12-word is
     * skipped - that returns 0, since 24-word SEEDTOOL_CARDS already covers
     * the case without needing replacement). */
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

/* seedtool_required_events' counts are chosen so that an honest run does not
 * merely clear the poor-entropy gate but reports at or above the minimum on
 * screen - the number the reader has just been asked to trust. That is a
 * stricter property than the false-positive bound above, which counts only
 * runs pushed past DICE_ENTROPY_TOLERANCE, and the gap between the two is
 * wide enough to hide a regression: at D6's former counts of 50 and 99 rolls
 * an honest run read short 21.9% and 36.6% of the time while being flagged
 * just 3.0% and 5.3%, so the bound above passed comfortably and protected
 * nothing. Whichever of the two properties a future change means to hold,
 * both are now pinned.
 *
 * Coin is left out by name rather than by oversight. One flip is exactly one
 * bit, so its count is at once the theoretical minimum and the entire
 * transcript buffer, and about a quarter of honest coin runs report a bit
 * short with no room left to pad them - the trade-off README's entropy
 * section spells out. This exists to keep the other sources from drifting
 * into that state, not to hold coin to a standard its encoding forbids. */
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

/* Both Monte-Carlo checks above bound the honest case: how often a good run is
 * wrongly flagged, and how often it reads short. Neither would notice a gate
 * that had stopped flagging anything at all. This is the other direction - a
 * source degraded far enough that its run genuinely carries fewer bits than
 * the seed needs must be caught.
 *
 * "Genuinely" is the load-bearing word, and getting it wrong is easy: a die
 * stuck on five of six faces still yields log2(5) = 2.32 bits a roll, so at
 * sixty rolls it carries 139 bits and passing it is the correct answer, not a
 * miss. Each case below is therefore chosen so the true entropy, `count *
 * log2(usable)`, falls below the minimum even before the estimator's error -
 * and the expected verdict is computed from that rather than written down. */
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

static size_t count_pixel_color(const uint16_t color)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    size_t count = 0;
    for (size_t i = 0; i < SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT; ++i) {
        if (pixels[i] == color) {
            ++count;
        }
    }
    return count;
}

/* Every one of the 2048 word numbers, drawn as a Stackbit 1248 grid, lights
 * exactly the punch cells its digits' bits call for — checked by counting
 * highlighted pixels rather than by duplicating the renderer's private
 * geometry, the same tactic dice_progress_bar_is_bounded uses. */
static bool stackbit_grid_is_sound(void)
{
    /* "0001" lights exactly one cell; its pixel count is the discovered
     * per-dot area rather than a hard-coded one. */
    seedtool_render_stackbit_screen("Stackbit 1248", 1, seedtool_word(0), "1/1");
    const size_t unit = count_pixel_color(0xfd20);
    if (!unit) {
        return false;
    }
    for (unsigned number = 1; number <= SEEDTOOL_WORDLIST_LEN; ++number) {
        char digits[5];
        (void)snprintf(digits, sizeof(digits), "%04u", number);
        unsigned bits = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned v = (unsigned)(digits[i] - '0');
            bits += (v & 1u) + ((v >> 1) & 1u) + ((v >> 2) & 1u) + ((v >> 3) & 1u);
        }
        seedtool_render_stackbit_screen("Stackbit 1248", number, seedtool_word(number - 1), "1/1");
        if (count_pixel_color(0xfd20) != bits * unit) {
            return false;
        }
    }
    return true;
}

/* Same proof as stackbit_grid_is_sound, for the layout that matches the
 * physical Stackbit 1248 plate's own two-row arrangement instead of the
 * simplified one: the total lit area is the same popcount-of-digits formula
 * either way, since every digit still contributes 0-4 lit weight cells
 * regardless of where on screen they are placed. */
static bool stackbit_physical_grid_is_sound(void)
{
    seedtool_render_stackbit_physical_screen("Stackbit 1248", 1, seedtool_word(0), "1/1");
    const size_t unit = count_pixel_color(0xfd20);
    if (!unit) {
        return false;
    }
    for (unsigned number = 1; number <= SEEDTOOL_WORDLIST_LEN; ++number) {
        char digits[5];
        (void)snprintf(digits, sizeof(digits), "%04u", number);
        unsigned bits = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned v = (unsigned)(digits[i] - '0');
            bits += (v & 1u) + ((v >> 1) & 1u) + ((v >> 2) & 1u) + ((v >> 3) & 1u);
        }
        seedtool_render_stackbit_physical_screen("Stackbit 1248", number, seedtool_word(number - 1), "1/1");
        if (count_pixel_color(0xfd20) != bits * unit) {
            return false;
        }
    }
    return true;
}

/* The Compact SeedQR payload is exactly the mnemonic's raw entropy: the
 * all-zero 12- and 24-word vectors must decode to 16 and 32 zero bytes
 * respectively and still fit within the QR version main/seedtool_render.c
 * caps byte-mode codes at (QR_VERSION, 6 there), and a broken checksum must be
 * rejected rather than silently encoded. Byte mode is drawn at the smallest
 * version that holds the payload, not always that cap — the whole point of
 * "compact" in the SeedSigner/Krux convention this follows — so 16 and 32
 * raw bytes must land on versions 1 and 2 respectively, not the cap itself. */
static bool compact_seedqr_is_sound(void)
{
    uint8_t entropy[32];
    size_t len = 0;
    if (seedtool_mnemonic_entropy(mnemonic, entropy, sizeof(entropy), &len) != SEEDTOOL_OK || len != 16) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (entropy[i] != 0) {
            return false;
        }
    }
    if (qrcode_versionForBytes(ECC_LOW, (uint16_t)len, 6) != 1) {
        return false;
    }
    if (!seedtool_render_qr_bytes("Compact SeedQR", entropy, len)) {
        return false;
    }
    if (seedtool_mnemonic_entropy(mnemonic24, entropy, sizeof(entropy), &len) != SEEDTOOL_OK || len != 32) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (entropy[i] != 0) {
            return false;
        }
    }
    if (qrcode_versionForBytes(ECC_LOW, (uint16_t)len, 6) != 2) {
        return false;
    }
    if (!seedtool_render_qr_bytes("Compact SeedQR", entropy, len)) {
        return false;
    }
    return seedtool_mnemonic_entropy(bad_checksum_mnemonic, entropy, sizeof(entropy), &len) != SEEDTOOL_OK;
}

/* seedtool_render_qr_bytes_regions must tile the exact QR sizes Compact
 * SeedQR ever produces (per compact_seedqr_is_sound above: version 1, 21x21
 * for 12 words; version 2, 25x25 for 24 words) with no gap or partial-region
 * edge case: 21 = 3*7 and 25 = 5*5 divide evenly by Krux's own region-size
 * thresholds. Every region index below that count must render, and the first
 * one at or past it must not -- the same "prove the count is exact, not
 * approximate" property compact_seedqr_is_sound already checks for QR
 * versions, applied to region tiling instead. */
static bool zoomed_qr_regions_are_sound(void)
{
    uint8_t entropy[32];
    size_t len = 0;

    if (seedtool_mnemonic_entropy(mnemonic, entropy, sizeof(entropy), &len) != SEEDTOOL_OK || len != 16) {
        return false;
    }
    if (seedtool_render_qr_bytes_regions(len) != 9) { /* 21x21 in 7x7 blocks: 3x3 */
        return false;
    }
    for (size_t i = 0; i < 9; ++i) {
        if (!seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, i)) {
            return false;
        }
    }
    if (seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, 9)) {
        return false;
    }

    if (seedtool_mnemonic_entropy(mnemonic24, entropy, sizeof(entropy), &len) != SEEDTOOL_OK || len != 32) {
        return false;
    }
    if (seedtool_render_qr_bytes_regions(len) != 25) { /* 25x25 in 5x5 blocks: 5x5 */
        return false;
    }
    for (size_t i = 0; i < 25; ++i) {
        if (!seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, i)) {
            return false;
        }
    }
    return !seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, 25);
}

/* The bar's warn (red) and complete (green) colors appear nowhere else on a
 * dice-entry screen, and its fill grows with the percentage passed in — so
 * both are checked by counting matching pixels rather than by duplicating the
 * renderer's private layout constants here. */
static bool dice_progress_bar_is_bounded(void)
{
    const uint16_t warn_color = 0xf800, go_color = 0x07e0, fill_color = 0xfd20;
    const char* const footer = "L/R move   BOTH select";
    const seedtool_progress_t low = { .rolls_pct = 10, .entropy_pct = 10, .warn = false, .complete = false };
    seedtool_render_dice_screen("D6 dice  1/50", "3", "123", footer, &low);
    if (count_pixel_color(warn_color) != 0 || count_pixel_color(go_color) != 0) {
        return false;
    }
    const size_t low_fill = count_pixel_color(fill_color);

    const seedtool_progress_t high = { .rolls_pct = 90, .entropy_pct = 90, .warn = false, .complete = false };
    seedtool_render_dice_screen("D6 dice  1/50", "3", "123", footer, &high);
    if (count_pixel_color(fill_color) <= low_fill) {
        return false;
    }

    const seedtool_progress_t warned = { .rolls_pct = 50, .entropy_pct = 50, .warn = true, .complete = false };
    seedtool_render_dice_screen("D6 dice  1/50", "3", "123", footer, &warned);
    if (count_pixel_color(warn_color) == 0) {
        return false;
    }

    const seedtool_progress_t done = { .rolls_pct = 100, .entropy_pct = 100, .warn = false, .complete = true };
    seedtool_render_dice_screen("D6 dice  1/50", "3", "123", footer, &done);
    return count_pixel_color(go_color) != 0;
}

/* The widest a title can be for one source at one word count: its longest
 * position counter (both numbers at seedtool_required_events' maximum) beside
 * the largest bit count that run can ever report. The bit ceiling is computed
 * from the same core functions the app grades with rather than written down
 * here, so a change to the estimator or to its Miller-Madow correction moves
 * this check with it instead of leaving it asserting a stale number. */
static bool title_fits(const seedtool_source_t source, const size_t sides, const size_t words, const char* label)
{
    const size_t required = seedtool_required_events(source, words);
    if (!required) {
        return true; /* this source/word-count combination is not offered */
    }
    int bits = 0;
    if (source == SEEDTOOL_CARDS) {
        /* Exact, not estimated, and the same for any draw of that length. */
        if (seedtool_card_entropy_bits(required, &bits) != SEEDTOOL_OK) {
            return false;
        }
    } else {
        /* The most uniform run possible at this length maximises the plug-in
         * estimator, and the bias correction the app adds is a constant. */
        uint8_t values[SEEDTOOL_MAX_EVENTS];
        for (size_t i = 0; i < required; ++i) {
            values[i] = (uint8_t)(i % sides) + 1;
        }
        if (seedtool_dice_entropy_bits(source, values, required, &bits) != SEEDTOOL_OK) {
            return false;
        }
        bits += (int)lround(seedtool_dice_entropy_bias_bits(source));
    }

    char title[48];
    /* Format copied from format_progress_heading() in seedtool_app.c. */
    (void)snprintf(title, sizeof(title), "%s %u/%u %d bits", label, (unsigned)required, (unsigned)required, bits);
    seedtool_render_dice_screen(title, "3", "123456", "L/R move   BOTH select", &(seedtool_progress_t) { 0 });

    /* Measured, not just tested for clipping: a title that merely reaches the
     * glass is already too wide to read at arm's length, and the version of
     * this heading that carried the minimum as well ("Coin flips 256/256
     * 257/256b") cleared the edge by five pixels while looking wedged against
     * it. The bar this title labels insets itself by DICE_BAR_MARGIN
     * (seedtool_render.c, kept in sync with the 20 below), so the title is
     * held to the same inset: no wider than the thing it describes. */
    const uint16_t* const pixels = seedtool_render_pixels();
    int leftmost = SEEDTOOL_DISPLAY_WIDTH, rightmost = -1;
    /* The 16px title face sits at y=5..~21; scanned a little wide either way. */
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000) {
                if (x < leftmost) {
                    leftmost = x;
                }
                if (x > rightmost) {
                    rightmost = x;
                }
            }
        }
    }
    if (rightmost < 0) {
        return false; /* nothing drawn at all - the title never reached the screen */
    }
    return leftmost >= 20 && rightmost <= SEEDTOOL_DISPLAY_WIDTH - 1 - 20;
}

/* format_progress_heading in seedtool_app.c appends the bits collected so far
 * to the entry screen's title, drawn at the 16px face - a different font from
 * the small one the line-2 hints below are checked against, so it gets its
 * own edge check rather than reusing theirs. Every label the app can put
 * there is covered: the four source names collect_entropy() passes, plus the
 * two the card screens substitute for them - enter_card titles its carousels
 * "Suit" and then the suit's own name, so a card run never shows "Cards" at
 * all, and "Diamonds" is the widest title either card mode can produce. */
static bool dice_screen_titles_clear_the_edges(void)
{
    static const size_t word_counts[] = { 12, 24 };
    for (size_t w = 0; w < sizeof(word_counts) / sizeof(word_counts[0]); ++w) {
        const size_t words = word_counts[w];
        if (!title_fits(SEEDTOOL_D6, 6, words, "D6 dice") || !title_fits(SEEDTOOL_D20, 20, words, "D20 dice")
            /* "Coins", not the "Coin flips" the menu list above is checked
             * with: collect_entropy() deliberately labels the entry screen
             * with the shorter name so the spelled-out "bits" fits. */
            || !title_fits(SEEDTOOL_COIN, 2, words, "Coins")) {
            return false;
        }
        /* CARD_SUIT_NAMES in seedtool_app.c, plus the suit carousel's own
         * title - copied here for the same reason the hints below are. */
        static const char* const card_labels[] = { "Suit", "Clubs", "Diamonds", "Hearts", "Spades" };
        for (size_t i = 0; i < sizeof(card_labels) / sizeof(card_labels[0]); ++i) {
            if (!title_fits(SEEDTOOL_CARDS, 52, words, card_labels[i])
                || !title_fits(SEEDTOOL_CARDS_REPLACE, 52, words, card_labels[i])) {
                return false;
            }
        }
    }
    return true;
}

/* Line 2 of a dice/card entry screen sits just above the quality bar (see
 * seedtool_render.c's DICE_BAR_Y, kept in sync with the 90..104 checked
 * below), with no margin to spare for a second wrapped line. "Return card,
 * reshuffle each draw" shipped long enough to wrap there, landing its second
 * line inside the bar instead of above it - checked here against every hint
 * the app actually shows, copied from collect_entropy() in seedtool_app.c,
 * so a future hint that grows past one line fails this instead of shipping. */
static bool dice_screen_hints_clear_the_bar(void)
{
    static const char* const hints[]
        = { "Red bar = non-random", "Return & reshuffle each card", "Looks good - generate?" };
    const char* const footer = "BOTH continue   Up/Down back";
    const seedtool_progress_t empty = { 0 };
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); ++i) {
        seedtool_render_dice_screen("Title", "99 cards needed", hints[i], footer, &empty);
        const uint16_t* const pixels = seedtool_render_pixels();
        for (int y = 90; y < 104; ++y) {
            for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
                if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] == 0xffff) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* The backup-confirmation screen show_generated() puts between the word list
 * and the quiz is a plain two-line acknowledge, so it has no quality bar to
 * wrap into - but a line too wide for this face still wraps down onto the one
 * below it, and line 2 wrapping lands on the footer. Both word counts are
 * rendered (the first line names how many words the quiz asks for, 4 of 12 or
 * 8 of 24) and each line is required to stay inside its own band. Strings
 * copied from show_generated() in seedtool_app.c, the same way the dice hints
 * above are. */
/* Bottom-most lit row with `text` drawn as line 1 of an otherwise bare screen.
 * draw_centered_box wraps at the display width rather than clipping, and the
 * body's two lines sit at fixed y, so a wrapped line 1 is drawn straight over
 * line 2 - it never runs off the screen, which is why measuring the edges
 * (the way the dice titles above are measured) would not see it. */
static int rendered_bottom(const char* text)
{
    seedtool_render_screen("Confirm backup", text, "", "");
    const uint16_t* const pixels = seedtool_render_pixels();
    int bottom = -1;
    for (int y = 0; y < SEEDTOOL_DISPLAY_HEIGHT; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000) {
                bottom = y;
                break;
            }
        }
    }
    return bottom;
}

static bool backup_confirm_screen_lines_do_not_wrap(void)
{
    /* Calibrated against a string that plainly fits, so this needs no access
     * to the font's private line height: anything reaching lower than one line
     * of text does has taken a second line. The reference carries descenders
     * because the strings below do - measured against an "X", whose glyph
     * stops three rows higher, every one of them would read as wrapped. */
    const int one_line = rendered_bottom("gjpqy");
    if (one_line < 0) {
        return false;
    }
    /* Both word counts the quiz can ask for (4 of 12, 8 of 24) and the line
     * below them, copied from show_generated() in seedtool_app.c the same way
     * the dice hints above are. */
    static const char* const lines[]
        = { "Retype 4 of the 12 words", "Retype 8 of the 24 words", "Have your backup ready" };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        if (rendered_bottom(lines[i]) > one_line) {
            return false;
        }
    }
    return true;
}

/* Lit rows of the nav chrome, scanned band by band. The chrome packs a title
 * bar, three list rows and a confirm bar into 135 pixels with single-digit
 * gaps between them, and draw_centered_box wraps rather than clips - so a
 * title or a button label one word too long lands in the band below instead
 * of running off the glass, exactly the failure the dice-hint check above
 * exists for. Bands are the geometry in seedtool_render.c (NAV_BACK_HEIGHT,
 * LIST_TOP, NAV_ROW_HEIGHT, NAV_BAR_Y), kept in sync with the numbers here. */
static bool nav_band_is_clear(const int from, const int to)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    for (int y = from; y < to; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000) {
                return false;
            }
        }
    }
    return true;
}

/* Nothing lit at or right of `x` in the title band. Measured on the right
 * rather than over the arrow on the left, because that is the side a title too
 * wide for its column shows up on: draw_centered_box centres by an offset it
 * clamps at zero, so an overlong line starts hard against its left edge and
 * runs off the right one. A title that clears this has cleared the arrow. */
static bool nav_title_stays_in_its_column(const int x)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    for (int y = 0; y < 20; ++y) {
        for (int column = x; column < SEEDTOOL_DISPLAY_WIDTH; ++column) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + column] != 0x0000) {
                return false;
            }
        }
    }
    return true;
}

/* The confirm bar's band (NAV_BAR_Y..the bottom of the glass), with its label
 * required to stay a few pixels clear of both edges. */
/* NAV_BAR_Y in seedtool_render.c, kept in sync with this. */
#define NAV_BAR_BAND_Y 118

static bool nav_bar_label_fits(void)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    int leftmost = SEEDTOOL_DISPLAY_WIDTH, rightmost = -1;
    for (int y = NAV_BAR_BAND_Y; y < SEEDTOOL_DISPLAY_HEIGHT; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] == 0x0000) {
                continue;
            }
            /* The rule drawn along the top of an unfilled bar spans the whole
             * width and is not the label; skipped by row, not by colour, so a
             * label sharing the rule's colour still counts. */
            if (y == NAV_BAR_BAND_Y) {
                continue;
            }
            if (x < leftmost) {
                leftmost = x;
            }
            if (x > rightmost) {
                rightmost = x;
            }
        }
    }
    if (rightmost < 0) {
        return false; /* the label never reached the screen */
    }
    return leftmost >= 4 && rightmost <= SEEDTOOL_DISPLAY_WIDTH - 5;
}

static bool nav_chrome_bands_do_not_collide(void)
{
    /* Twelve rows of the widest label review_and_confirm can build: two
     * digits, a dot, a space and the longest word in the BIP39 list. */
    static const char* const words[12] = { "01. wildcat", "02. abandon", "03. zoo", "04. mosquito", "05. jealous",
        "06. abandon", "07. abandon", "08. abandon", "09. abandon", "10. abandon", "11. abandon", "12. abandon" };
    /* Both titles review_and_confirm shows, and the label on its confirm bar,
     * copied from seedtool_app.c the same way the dice hints above are. */
    static const char* const titles[] = { "Review words", "Review - fix a word" };
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); ++i) {
        /* Every state the chrome can be drawn in, since each paints something
         * the others do not: the arrow highlighted, an item highlighted with
         * the confirm bar dimmed, and the confirm bar filled. */
        static const size_t selections[] = { SEEDTOOL_NAV_BACK, 0, SEEDTOOL_NAV_CONFIRM };
        for (size_t s = 0; s < sizeof(selections) / sizeof(selections[0]); ++s) {
            /* Item 0 stands for the invalid-checksum screen, the one case
             * where a word is the selection and the bar cannot be taken - so
             * that pass is the one that draws the bar dimmed. */
            const bool confirm_enabled = selections[s] != 0;
            seedtool_render_nav_list(titles[i], words, 12, selections[s], 0, "Continue", confirm_enabled);
            /* Title bar ends at 20, rows run 21..116 (LIST_TOP + 3 *
             * NAV_ROW_HEIGHT, less the 2px each row leaves under itself), the
             * confirm bar starts at 118. Two gaps must stay dark, or a band
             * has grown into its neighbour. */
            if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(115, 118)) {
                return false;
            }
            /* The title's column is inset by NAV_BACK_X + NAV_BACK_WIDTH at
             * both ends, so its right edge is that far in from the glass. */
            if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
                return false;
            }
            /* The label on the bar, measured only in the states where the
             * bar is not filled - a filled bar lights every pixel of the band
             * by design, including the bottom row, and would swamp this. The
             * label is drawn on one line by draw_centered_in, which does not
             * wrap, so an overlong one runs off the right edge instead of
             * down; measured on both sides so it is held clear of the glass
             * rather than merely on it. */
            if (selections[s] != SEEDTOOL_NAV_CONFIRM && !nav_bar_label_fits()) {
                return false;
            }
        }
    }
    /* The text screen wearing the same chrome: enter_word_number's check of
     * the number just typed. Its body is drawn by draw_centered, which wraps
     * rather than clips, so line 2 growing to two lines would march down
     * towards the bar - and the bar's own label is the widest this chrome
     * carries anywhere. Strings copied from seedtool_app.c, the same way the
     * dice hints above are. */
    /* A menu wears the chrome with no confirm bar at all: its rows are its
     * actions, so the arrow is the only control and the bar's band must stay
     * dark rather than holding an empty outline. */
    for (size_t s = 0; s < 2; ++s) {
        seedtool_render_nav_list("Word entry", words, 2, s ? SEEDTOOL_NAV_BACK : 0, 0, NULL, false);
        if (!nav_band_is_clear(NAV_BAR_BAND_Y, SEEDTOOL_DISPLAY_HEIGHT)) {
            return false;
        }
    }
    static const struct {
        const char* title;
        const char* line1;
        const char* line2;
        const char* label;
    } screens[] = {
        /* Every screen on the chrome, with the widest text each can build -
         * the longest word in the list, and every digit of the largest word
         * number twice over. Copied from seedtool_app.c the same way the dice
         * hints above are. */
        { "Word 12/24", "mosquito", "Number 2048 of 2048", "Use this word" },
        { "Confirm backup", "Retype 8 of the 24 words", "Have your backup ready", "Start quiz" },
        { "Checksum valid", "BIP39 English", "Derivation unlocked", "Open wallet" },
        { "Word doesn't match", "Check your backup", NULL, "Try again" },
        { "Compact SeedQR", "Encodes your ENTIRE seed", "A photo = total loss of funds", "Show QR" },
        { "QR export", "Account key included", "A photo reveals every address", "Show QR" },
        { "Descriptor export", "Account key included", "A photo reveals every address", "Show descriptor" },
        { "Confirm passphrase", "Enter it a second time", "Exact match required", "Enter again" },
        { "Poor entropy!", "128 of 128 bits", NULL, "Proceed anyway" },
        { "Pattern detected!", NULL, NULL, "Proceed anyway" },
    };
    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); ++i) {
        for (int on_back = 0; on_back < 2; ++on_back) {
            seedtool_render_nav_screen(
                screens[i].title, screens[i].line1, screens[i].line2, on_back, screens[i].label);
            /* Line 2 sits at 65 and the 16px face is that tall again, so its
             * wraps land at 81, 97, 113 - and 113 is inside the bar's own
             * margin. One wrap is tolerated because the export warnings
             * already take it on the plain screen this chrome inherits its
             * body layout from ("A photo reveals every address" has always
             * been two lines there); a second is what would reach the bar. */
            if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(97, 118)) {
                return false;
            }
            if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
                return false;
            }
            /* Measured only with the arrow selected, for the same reason as
             * above: a selected bar is filled and would swamp this. */
            if (on_back && !nav_bar_label_fits()) {
                return false;
            }
        }
    }
    /* The same chrome with the entropy quality bar drawn in. Its band is
     * different: the bar occupies DICE_BAR_Y=90..103, so what is checked here
     * is the 104..118 gap between it and the confirm bar. A body line wrapping
     * *into* the quality bar is not checked here and does not need to be -
     * dice_screen_hints_clear_the_bar above already holds these same hints to
     * that, at the same body heights this chrome inherits. */
    static const struct {
        const char* title;
        const char* line1;
        const char* line2;
        const char* label;
    } dice[] = {
        { "Coins", "256 flips needed", "Red bar = non-random", "Start" },
        { "D6 dice", "99 rolls needed", "Red bar = non-random", "Start" },
        { "Diamonds", "99 cards needed", "Return & reshuffle each card", "Start" },
        { "Coins", "256 of 256 bits", "Looks good", "Generate seed" },
    };
    const seedtool_progress_t full = { .rolls_pct = 100, .entropy_pct = 100, .warn = false, .complete = true };
    for (size_t i = 0; i < sizeof(dice) / sizeof(dice[0]); ++i) {
        for (int on_back = 0; on_back < 2; ++on_back) {
            seedtool_render_nav_dice_screen(
                dice[i].title, dice[i].line1, dice[i].line2, on_back, dice[i].label, &full);
            if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(104, 118)) {
                return false;
            }
            if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
                return false;
            }
            if (on_back && !nav_bar_label_fits()) {
                return false;
            }
        }
    }
    /* The notices: no arrow, so the title centres across the whole glass and
     * has the full width to fit in - but the bar is still there, and the body
     * still wraps rather than clips. Strings from seedtool_app.c. */
    static const struct {
        const char* title;
        const char* line1;
        const char* line2;
        const char* label;
    } notices[] = {
        { "Invalid checksum", "Check your words", "Fix one to continue", "Fix a word" },
        { "Passphrase mismatch", "Nothing was derived", "Try again", "Try again" },
        { "Backup confirmed", "Words matched", NULL, "Continue" },
        { "Too long for a QR", "Compact SeedQR", "Read it as text instead", "OK" },
        { "Error", "Could not derive addresses", NULL, "OK" },
        { "Error", "Could not compute", "word numbers", "OK" },
    };
    for (size_t i = 0; i < sizeof(notices) / sizeof(notices[0]); ++i) {
        seedtool_render_nav_notice(notices[i].title, notices[i].line1, notices[i].line2, notices[i].label);
        if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(97, NAV_BAR_BAND_Y)) {
            return false;
        }
    }
    /* The paged screens. Their bodies run lower than the two-line ones -
     * screen4's fourth row ends at 103 - and the page counter goes in what is
     * left, so the band that must stay clear is only 116..118. Body lines are
     * already held to their own widths by seedtool_render_fit, which is what
     * page_text splits by; what is new here is the counter, and whether the
     * two together clear the bar. */
    static const size_t cursors[] = { SEEDTOOL_NAV_BACK, 0, SEEDTOOL_NAV_CONFIRM };
    for (size_t c = 0; c < sizeof(cursors) / sizeof(cursors[0]); ++c) {
        /* A full-width transcript line, and the widest counter either paged
         * screen can reach: MAX_PAGE_LINES=24 gives page_text eight pages. */
        seedtool_render_nav_screen3("Canonical transcript", "20-1-2-1-4-1-1-1-1-1-1-1-1-",
            "1-1-1-1-1-1-1-1-1-20-1-1-1-", "1-1-1-1-1-1-1-1-1-1", cursors[c], "Continue", "8/8");
        if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(116, 118)) {
            return false;
        }
        if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
            return false;
        }
        if (cursors[c] != SEEDTOOL_NAV_CONFIRM && !nav_bar_label_fits()) {
            return false;
        }
        seedtool_render_nav_screen4("BIP39 word numbers", "21. mosquito", "22. mosquito", "23. mosquito",
            "24. mosquito", cursors[c], "Continue", "6/6");
        if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(116, 118)) {
            return false;
        }
        if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
            return false;
        }
        if (cursors[c] != SEEDTOOL_NAV_CONFIRM && !nav_bar_label_fits()) {
            return false;
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

/* Inverse of seedtool_bbqr_base32_encode, for the round-trip check below only
 * -- not part of the firmware, which only ever needs to encode (a QR reader
 * does the decoding). */
static bool base32_decode(const char* text, const size_t len, uint8_t* out, const size_t out_cap, size_t* out_len)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint32_t buffer = 0;
    unsigned bits = 0;
    size_t written = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!text[i]) {
            return false;
        }
        const char* const found = strchr(alphabet, text[i]);
        if (!found) {
            return false;
        }
        buffer = (buffer << 5) | (uint32_t)(found - alphabet);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (written >= out_cap) {
                return false;
            }
            out[written++] = (uint8_t)((buffer >> bits) & 0xff);
        }
    }
    *out_len = written;
    return true;
}

/* An account key's worth of payload, split into whatever
 * seedtool_render_qr_alphanumeric_capacity() says fits one frame, must
 * reassemble byte for byte: every part's header must carry the right
 * "B$2U" + total + index, every part's base32 payload must decode cleanly,
 * and concatenating them in index order must reproduce the original bytes --
 * the same property a real BBQr-reading wallet depends on, checked here
 * without needing one. */
static bool bbqr_parts_are_sound(void)
{
    const char payload[] = "[65fb43fe/84'/0'/0']"
                            "xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuyvz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4"
                            "ZeZXYVUhLv1VMrjPC7PW6V";
    const size_t len = strlen(payload);
    const uint8_t bbqr_frame_max_version = 3; /* BBQR_FRAME_MAX_VERSION in main/seedtool_app.c */
    const size_t frame_chars = seedtool_render_qr_alphanumeric_capacity(bbqr_frame_max_version);
    const size_t parts = seedtool_bbqr_part_count(len, frame_chars);
    if (!parts) {
        return false;
    }

    uint8_t reassembled[sizeof(payload)];
    size_t reassembled_len = 0;
    for (size_t i = 0; i < parts; ++i) {
        char frame[512];
        if (!seedtool_bbqr_part((const uint8_t*)payload, len, '2', 'U', i, parts, frame, sizeof(frame))) {
            return false;
        }
        const size_t frame_len = strlen(frame);
        if (frame_len > frame_chars || frame_len < SEEDTOOL_BBQR_HEADER_LEN) {
            return false;
        }
        if (frame[0] != 'B' || frame[1] != '$' || frame[2] != '2' || frame[3] != 'U') {
            return false;
        }
        char total_digits[3] = { frame[4], frame[5], '\0' };
        char index_digits[3] = { frame[6], frame[7], '\0' };
        if ((size_t)strtol(total_digits, NULL, 36) != parts || (size_t)strtol(index_digits, NULL, 36) != i) {
            return false;
        }
        uint8_t chunk[256];
        size_t chunk_len = 0;
        if (!base32_decode(frame + SEEDTOOL_BBQR_HEADER_LEN, frame_len - SEEDTOOL_BBQR_HEADER_LEN, chunk,
                sizeof(chunk), &chunk_len)) {
            return false;
        }
        if (reassembled_len + chunk_len > sizeof(reassembled)) {
            return false;
        }
        memcpy(reassembled + reassembled_len, chunk, chunk_len);
        reassembled_len += chunk_len;
    }
    return reassembled_len == len && memcmp(reassembled, payload, len) == 0;
}

static int self_test(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    if (wally_init(0) != WALLY_OK || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_RECEIVE, 0, address, sizeof(address))
            != SEEDTOOL_OK
        || strcmp(address, expected_address) != 0
        || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_CHANGE, 0, address, sizeof(address))
            != SEEDTOOL_OK
        || strcmp(address, expected_change_address) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub84) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, 0, SEEDTOOL_XPUB, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub86) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_zpub84) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, 0, SEEDTOOL_ZPUB, xpub, sizeof(xpub))
            != SEEDTOOL_EINVAL) {
        fputs("Origo host self-test failed\n", stderr);
        return 1;
    }
    /* Account really is threaded into the derivation, not silently ignored -
     * a different account must give a different xpub - and it is bounded:
     * SEEDTOOL_MAX_ACCOUNT_INDEX + 1 is rejected outright. */
    {
        char xpub_account_zero[SEEDTOOL_MAX_XPUB_LEN];
        char xpub_account_one[SEEDTOOL_MAX_XPUB_LEN];
        if (seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_XPUB, xpub_account_zero,
                sizeof(xpub_account_zero))
                != SEEDTOOL_OK
            || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, 1, SEEDTOOL_XPUB, xpub_account_one,
                   sizeof(xpub_account_one))
                != SEEDTOOL_OK
            || strcmp(xpub_account_zero, xpub_account_one) == 0
            || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, SEEDTOOL_MAX_ACCOUNT_INDEX + 1, SEEDTOOL_XPUB,
                   xpub_account_one, sizeof(xpub_account_one))
                != SEEDTOOL_EINVAL) {
            fputs("Origo account index self-test failed\n", stderr);
            return 1;
        }
    }
    /* The same for the chain level: the branch really is threaded down into
     * the derivation rather than ignored - receive and change at one index
     * must differ - and a value outside seedtool_chain_t is rejected outright
     * rather than written into the path on trust. */
    {
        char receive[SEEDTOOL_MAX_ADDRESS_LEN];
        char change[SEEDTOOL_MAX_ADDRESS_LEN];
        char batch[2][SEEDTOOL_MAX_ADDRESS_LEN];
        if (seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP86, 0, SEEDTOOL_RECEIVE, 3, receive, sizeof(receive))
                != SEEDTOOL_OK
            || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP86, 0, SEEDTOOL_CHANGE, 3, change, sizeof(change))
                != SEEDTOOL_OK
            || strcmp(receive, change) == 0
            || seedtool_mainnet_address(
                   mnemonic, "", SEEDTOOL_BIP84, 0, (seedtool_chain_t)2, 0, receive, sizeof(receive))
                != SEEDTOOL_EINVAL
            || seedtool_mainnet_addresses(mnemonic, "", SEEDTOOL_BIP84, 0, (seedtool_chain_t)2, 2, batch)
                != SEEDTOOL_EINVAL) {
            fputs("Origo chain self-test failed\n", stderr);
            return 1;
        }
        /* The batch path derives the same branch the single-address one does,
         * rather than the two agreeing only on receive. */
        if (seedtool_mainnet_addresses(mnemonic, "", SEEDTOOL_BIP84, 0, SEEDTOOL_CHANGE, 2, batch) != SEEDTOOL_OK
            || strcmp(batch[0], expected_change_address) != 0) {
            fputs("Origo chain batch self-test failed\n", stderr);
            return 1;
        }
    }
    if (!wordlist_completion_is_sound()) {
        fputs("Origo wordlist completion self-test failed\n", stderr);
        return 1;
    }
    if (!word_numbers_are_reachable()) {
        fputs("Origo word number self-test failed\n", stderr);
        return 1;
    }
    if (!word_numbers_round_trip_is_sound()) {
        fputs("Origo word number round-trip self-test failed\n", stderr);
        return 1;
    }
    if (!stackbit_grid_is_sound()) {
        fputs("Origo Stackbit grid self-test failed\n", stderr);
        return 1;
    }
    if (!stackbit_physical_grid_is_sound()) {
        fputs("Origo physical Stackbit grid self-test failed\n", stderr);
        return 1;
    }
    if (!compact_seedqr_is_sound()) {
        fputs("Origo Compact SeedQR self-test failed\n", stderr);
        return 1;
    }
    if (!zoomed_qr_regions_are_sound()) {
        fputs("Origo zoomed QR regions self-test failed\n", stderr);
        return 1;
    }
    if (!bbqr_base32_is_sound()) {
        fputs("Origo BBQr base32 self-test failed\n", stderr);
        return 1;
    }
    if (!bbqr_parts_are_sound()) {
        fputs("Origo BBQr parts self-test failed\n", stderr);
        return 1;
    }
    if (!list_viewport_is_sound() || !labels_fit_a_row()) {
        fputs("Origo choice list self-test failed\n", stderr);
        return 1;
    }
    if (!layouts_are_complete()) {
        fputs("Origo keyboard layout self-test failed\n", stderr);
        return 1;
    }
    if (!wire_order_is_big_endian()) {
        fputs("Origo panel byte order self-test failed\n", stderr);
        return 1;
    }
    if (!partial_transcripts_are_prefixes()) {
        fputs("Origo running transcript self-test failed\n", stderr);
        return 1;
    }
    if (!full_entropy_events_generate()) {
        fputs("Origo full-event-count generation self-test failed\n", stderr);
        return 1;
    }
    if (!descriptor_checksum_matches_bip380_vector()) {
        fputs("Origo descriptor checksum self-test failed\n", stderr);
        return 1;
    }
    if (!dice_quality_is_sound()) {
        fputs("Origo dice entropy quality self-test failed\n", stderr);
        return 1;
    }
    if (!card_quality_is_sound()) {
        fputs("Origo card entropy quality self-test failed\n", stderr);
        return 1;
    }
    if (!dice_entropy_false_positive_rate_is_bounded()) {
        fputs("Origo dice entropy false-positive-rate self-test failed\n", stderr);
        return 1;
    }
    if (!honest_runs_report_at_least_the_minimum()) {
        fputs("Origo honest-run short-read self-test failed\n", stderr);
        return 1;
    }
    if (!insufficient_runs_are_flagged()) {
        fputs("Origo insufficient-run detection self-test failed\n", stderr);
        return 1;
    }
    if (!required_events_fit_the_collection_buffers()) {
        fputs("Origo collection buffer bound self-test failed\n", stderr);
        return 1;
    }
    if (!dice_progress_bar_is_bounded()) {
        fputs("Origo dice progress bar self-test failed\n", stderr);
        return 1;
    }
    if (!dice_screen_hints_clear_the_bar()) {
        fputs("Origo dice screen hint self-test failed\n", stderr);
        return 1;
    }
    if (!dice_screen_titles_clear_the_edges()) {
        fputs("Origo dice screen title self-test failed\n", stderr);
        return 1;
    }
    if (!backup_confirm_screen_lines_do_not_wrap()) {
        fputs("Origo backup confirmation screen self-test failed\n", stderr);
        return 1;
    }
    if (!nav_chrome_bands_do_not_collide()) {
        fputs("Origo nav chrome geometry self-test failed\n", stderr);
        return 1;
    }
    /* Every value the QR screen offers must actually encode. The account key
     * payload is the longest thing the device ever puts in a code - and with
     * the account index now user-chosen rather than always "0", the account
     * component itself can grow to SEEDTOOL_MAX_ACCOUNT_INDEX's own three
     * digits, landing right at this encoder's 134-byte ceiling rather than
     * comfortably under it. */
    for (size_t i = 0; i < 2; ++i) {
        char payload[160];
        const unsigned purpose = i ? 86 : 84;
        (void)snprintf(payload, sizeof(payload), "[73c5da0a/%u'/0'/%u']%s", purpose, SEEDTOOL_MAX_ACCOUNT_INDEX,
            i ? expected_xpub86 : expected_xpub84);
        if (strlen(payload) > 134 || !seedtool_render_qr("BIP84 account key", payload)) {
            fputs("Origo account key QR self-test failed\n", stderr);
            return 1;
        }
    }
    if (!paging_is_lossless(expected_xpub84) || !paging_is_lossless(expected_xpub86)
        || !paging_is_lossless(expected_address) || !paging_is_lossless(mnemonic)
        || !paging_is_lossless("cards-v1:ACKS7D2H")) {
        fputs("Origo paging self-test failed\n", stderr);
        return 1;
    }
    seedtool_render_screen("ORIGO", "HOST SELF-TEST", address, "OK");
    seedtool_render_splash();
    if (!seedtool_render_qr("BIP84 address 0", address)) {
        fputs("Origo QR self-test failed\n", stderr);
        return 1;
    }
    /* Draw a list scrolled to its end and one scrolled to its start, so both
     * arrows and the selection bar are exercised. */
    seedtool_render_list("Wallet", menu_labels, 7, 6, 4, "7/7   L/R move   BOTH select");
    seedtool_render_list("Word 3/12  aba", menu_labels, MENU_LABEL_COUNT, 0, 0, "1/24   L/R move   BOTH select");
    /* Draw the real layouts, each opened at its centre, so one that overflows a
     * row is caught here rather than on the device. */
    const bool letters[SEEDTOOL_LETTERS + 1] = { true };
    seedtool_render_keyboard("Word 1/12", "aba", SEEDTOOL_WORD_LAYOUT, letters,
        seedtool_layout_center(SEEDTOOL_WORD_LAYOUT), 1, 12);
    seedtool_render_keyboard("Word 12/12", "4", SEEDTOOL_WORD_NUMBER_LAYOUT, NULL,
        seedtool_layout_center(SEEDTOOL_WORD_NUMBER_LAYOUT), 12, 12);
    for (size_t page = 0; page < SEEDTOOL_PASSPHRASE_PAGES; ++page) {
        seedtool_render_keyboard("BIP39 passphrase", "", seedtool_passphrase_layouts[page], NULL,
            seedtool_layout_center(seedtool_passphrase_layouts[page]), 0, 0);
    }
    seedtool_zero(address, sizeof(address));
    seedtool_zero(xpub, sizeof(xpub));
    wally_cleanup(0);
    puts("Origo host self-test OK");
    return 0;
}

int main(const int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return self_test();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
        return 2;
    }
    seedtool_run();
}
