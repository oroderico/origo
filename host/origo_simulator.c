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

/* Every label the choice list can show. A label wider than a row is cut at the
 * last glyph that fits; nothing here is transcribed, but a silently shortened
 * label still misnames what the buttons are about to do. */
static const char* const menu_labels[] = { "Master fingerprint", "Account xpub BIP84", "Account xpub BIP86",
    "Address BIP84 bc1q", "Address BIP86 bc1p", "Address index: 99", "Done / erase", "Create Seed", "Restore Seed",
    "Complete Checksum", "About / Safety", "Reboot", "11 words + 7 coins", "23 words + 3 coins", "No passphrase",
    "Enter passphrase", "D6 dice", "D20 dice", "Coin flips", "Cards", "Back", "12 words", "24 words", "[delete]", "[back]", "Type the letters", "Enter word numbers" };
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
    for (size_t count = 1; count <= 40; ++count) {
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

static int self_test(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    if (wally_init(0) != WALLY_OK || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP84, 0, address, sizeof(address)) != SEEDTOOL_OK
        || strcmp(address, expected_address) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub84) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub86) != 0) {
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
