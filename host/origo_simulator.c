#include "seedtool_app.h"
#include "seedtool_core.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

/* Published BIP84/BIP86 vectors for the all-zero entropy mnemonic. */
static const char mnemonic[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
static const char expected_address[] = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
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
    "Account key", "Addresses", "Account key format", "xpub", "zpub", "Done / erase", "Create Seed", "Restore Seed",
    "Complete Checksum", "About / Safety", "Reboot", "11 words + 7 coins", "23 words + 3 coins", "No passphrase",
    "Enter passphrase", "D6 dice", "D20 dice", "Coin flips", "Cards", "Back", "12 words", "24 words", "[delete]", "[back]", "Type the letters", "Enter word numbers",
    "Backup", "Stackbit 1248", "Compact SeedQR", "Simple grid", "Physical layout" };
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
    /* 101 is the largest list the firmware builds: 100 addresses plus Back. */
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
    static const seedtool_source_t sources[] = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_COIN, SEEDTOOL_CARDS };
    static const uint8_t limits[] = { 6, 20, 1, 51 };
    for (size_t s = 0; s < 4; ++s) {
        uint8_t values[52];
        const size_t count = sources[s] == SEEDTOOL_CARDS ? 25 : 40;
        for (size_t i = 0; i < count; ++i) {
            values[i] = sources[s] == SEEDTOOL_CARDS
                ? (uint8_t)(i * 2 + 1)
                : (uint8_t)((sources[s] == SEEDTOOL_COIN ? 0 : 1) + (i * 7 + 3) % limits[s]);
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

    /* Defined only for D6/D20. */
    const uint8_t coin[2] = { 0, 1 };
    return seedtool_dice_entropy_bits(SEEDTOOL_COIN, coin, sizeof(coin), &bits) == SEEDTOOL_EINVAL;
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
 * respectively and still fit the pinned QR version, and a broken checksum
 * must be rejected rather than silently encoded. */
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
    if (!seedtool_render_qr_bytes("Compact SeedQR", entropy, len)) {
        return false;
    }
    return seedtool_mnemonic_entropy(bad_checksum_mnemonic, entropy, sizeof(entropy), &len) != SEEDTOOL_OK;
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

static int self_test(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    if (wally_init(0) != WALLY_OK || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP84, 0, address, sizeof(address)) != SEEDTOOL_OK
        || strcmp(address, expected_address) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, SEEDTOOL_XPUB, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub84) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, SEEDTOOL_XPUB, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub86) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_zpub84) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, SEEDTOOL_ZPUB, xpub, sizeof(xpub)) != SEEDTOOL_EINVAL) {
        fputs("Origo host self-test failed\n", stderr);
        return 1;
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
    if (!dice_quality_is_sound()) {
        fputs("Origo dice entropy quality self-test failed\n", stderr);
        return 1;
    }
    if (!dice_progress_bar_is_bounded()) {
        fputs("Origo dice progress bar self-test failed\n", stderr);
        return 1;
    }
    /* Every value the QR screen offers must actually encode. The account key
     * payload is the longest thing the device ever puts in a code. */
    for (size_t i = 0; i < 2; ++i) {
        char payload[160];
        const unsigned purpose = i ? 86 : 84;
        (void)snprintf(payload, sizeof(payload), "[73c5da0a/%u'/0'/0']%s", purpose, i ? expected_xpub86 : expected_xpub84);
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
    seedtool_render_keyboard(
        "Word 1/12", "aba", SEEDTOOL_WORD_LAYOUT, letters, seedtool_layout_center(SEEDTOOL_WORD_LAYOUT));
    seedtool_render_keyboard("Word 12/12", "4", SEEDTOOL_WORD_NUMBER_LAYOUT, NULL,
        seedtool_layout_center(SEEDTOOL_WORD_NUMBER_LAYOUT));
    for (size_t page = 0; page < SEEDTOOL_PASSPHRASE_PAGES; ++page) {
        seedtool_render_keyboard("BIP39 passphrase", "", seedtool_passphrase_layouts[page], NULL,
            seedtool_layout_center(seedtool_passphrase_layouts[page]));
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
