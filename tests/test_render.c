/* What the screens show: that the value drawn is the value held, and that the
 * chrome, QR codes and keyboards lay out as intended. */
#include "check.h"
#include "vectors.h"

#include "qrcode.h"
#include "seedtool_bbqr.h"
#include "seedtool_core.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

/* The panel takes each pixel most significant byte first, the framebuffer the
 * other way. The SDL simulator replaces the driver, so the conversion is pinned
 * here, including the colours whose symmetry hides a missing swap. */
static bool wire_order_is_big_endian(void)
{
    static uint16_t wire[SEEDTOOL_DISPLAY_WIDTH * 5];
    const char* const items[] = { "Create Seed", "Restore Seed", "Complete Checksum" };
    /* The Origo menu: chrome, no arrow, the selection on the first row, 226
     * orange pixels on row 21. */
    const seedtool_nav_t nav = { .selected = 0,
        .confirm_enabled = false,
        .back = false };

    seedtool_render_nav_list(&nav, "Origo", items, 3, 0);
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
    seedtool_render_nav_list(&nav, "Origo", items, 3, 0);
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

/* Hand-edited layouts are checked: no row wider than the renderer draws, and a
 * centre that is a real key. */
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

/* Every letter exactly once, and exactly one backspace: a row with a letter
 * dropped would make some BIP39 words impossible to type. */
static bool word_layout_is_complete(const char* const layout)
{
    bool seen[128] = { false };
    if (!layout_is_well_formed(layout) || seedtool_layout_keys(layout) != SEEDTOOL_LETTERS + 1) {
        return false;
    }
    for (size_t i = 0; i < seedtool_layout_keys(layout); ++i) {
        const unsigned char key = (unsigned char)seedtool_layout_key(layout, i);
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
    return true;
}

/* One arrangement's pages reach all 95 printable characters, so a passphrase
 * can always be retyped, under either setting. */
static bool passphrase_pages_are_complete(const char* const* const pages)
{
    bool seen[128] = { false };
    for (size_t page = 0; page < SEEDTOOL_PASSPHRASE_PAGES; ++page) {
        if (!layout_is_well_formed(pages[page])) {
            return false;
        }
        for (size_t i = 0; i < seedtool_layout_keys(pages[page]); ++i) {
            seen[(unsigned char)seedtool_layout_key(pages[page], i)] = true;
        }
    }
    for (unsigned char character = 0x20; character <= 0x7e; ++character) {
        if (!seen[character]) {
            return false;
        }
    }
    return true;
}

static bool layouts_are_complete(void)
{
    bool seen[128] = { false };

    if (!word_layout_is_complete(SEEDTOOL_WORD_LAYOUT) || !word_layout_is_complete(SEEDTOOL_WORD_LAYOUT_ABC)
        || !layout_is_well_formed(SEEDTOOL_WORD_NUMBER_LAYOUT)
        || seedtool_layout_keys(SEEDTOOL_WORD_NUMBER_LAYOUT) != SEEDTOOL_DIGITS + 2) {
        return false;
    }
    /* The number keyboard's characters, not just its count: enter_word_number
     * indexes reachable[key - '0'], so a non-digit would read past the array. */
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
    return passphrase_pages_are_complete(seedtool_passphrase_layouts)
        && passphrase_pages_are_complete(seedtool_passphrase_layouts_abc);
}

/* The selection is always on screen and the end of a list looks like the end,
 * for every list size and scroll position. */
static bool list_viewport_is_sound(void)
{
    const int track = 99;
    /* Well past the largest list the firmware builds (50 addresses and Go to
     * index). */
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

/* A partial transcript must be a prefix of the finished one, or the reader
 * checked a string the device then rewrote. Every prefix of every source. */
static bool partial_transcripts_are_prefixes(void)
{
    static const seedtool_source_t sources[]
        = { SEEDTOOL_D6, SEEDTOOL_D20, SEEDTOOL_COIN, SEEDTOOL_CARDS, SEEDTOOL_CARDS_REPLACE };
    static const uint8_t limits[] = { 6, 20, 1, 51, 51 };
    for (size_t s = 0; s < 5; ++s) {
        uint8_t values[52];
        const size_t count = sources[s] == SEEDTOOL_CARDS ? 25 : sources[s] == SEEDTOOL_CARDS_REPLACE ? 48 : 40;
        for (size_t i = 0; i < count; ++i) {
            /* Repeats here must be accepted, unlike SEEDTOOL_CARDS. */
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

/* Paging splits by pixel width, so the chunks must reassemble byte for byte. */
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

/* The cursor on a word, not the arrow: the proofs count COLOR_HIGHLIGHT pixels,
 * and a selected arrow is filled with it. */
static seedtool_nav_t stackbit_test_nav(void)
{
    const seedtool_nav_t nav = { .selected = SEEDTOOL_NAV_BODY,
        .confirm_enabled = false,
        .back = true };
    return nav;
}

/* All 2048 word numbers as Stackbit 1248 grids light exactly the cells their
 * digits' bits call for, measured by counting highlighted pixels. */
static bool stackbit_grid_is_sound(void)
{
    const seedtool_nav_t nav = stackbit_test_nav();
    /* "0001" lights exactly one cell; its pixel count is the discovered
     * per-dot area rather than a hard-coded one. */
    seedtool_render_stackbit_screen(&nav, "Stackbit 1248", 1, seedtool_word(0), "1/1");
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
        seedtool_render_stackbit_screen(&nav, "Stackbit 1248", number, seedtool_word(number - 1), "1/1");
        if (count_pixel_color(0xfd20) != bits * unit) {
            return false;
        }
    }
    return true;
}

/* The same proof for the physical plate's layout: the lit area is the same
 * popcount either way. */
static bool stackbit_physical_grid_is_sound(void)
{
    const seedtool_nav_t nav = stackbit_test_nav();
    seedtool_render_stackbit_physical_screen(&nav, "Stackbit 1248", 1, seedtool_word(0), "1/1");
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
        seedtool_render_stackbit_physical_screen(&nav, "Stackbit 1248", number, seedtool_word(number - 1), "1/1");
        if (count_pixel_color(0xfd20) != bits * unit) {
            return false;
        }
    }
    return true;
}

/* The Compact SeedQR payload is the raw entropy: the all-zero vectors decode to
 * 16 and 32 zero bytes on versions 1 and 2, and a bad checksum is refused. */
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
    if (!seedtool_render_qr_bytes("Compact SeedQR", entropy, len, SEEDTOOL_NAV_BACK)) {
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
    if (!seedtool_render_qr_bytes("Compact SeedQR", entropy, len, SEEDTOOL_NAV_BACK)) {
        return false;
    }
    return seedtool_mnemonic_entropy(bad_checksum_mnemonic, entropy, sizeof(entropy), &len) != SEEDTOOL_OK;
}

/* Region tiling is exact for both Compact SeedQR sizes (21 = 3 x 7, 25 = 5 x
 * 5): every index below the count renders and the first past it does not. */
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
        if (!seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, i, SEEDTOOL_NAV_BACK)) {
            return false;
        }
    }
    if (seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, 9, SEEDTOOL_NAV_BACK)) {
        return false;
    }

    if (seedtool_mnemonic_entropy(mnemonic24, entropy, sizeof(entropy), &len) != SEEDTOOL_OK || len != 32) {
        return false;
    }
    if (seedtool_render_qr_bytes_regions(len) != 25) { /* 25x25 in 5x5 blocks: 5x5 */
        return false;
    }
    for (size_t i = 0; i < 25; ++i) {
        if (!seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, i, SEEDTOOL_NAV_BACK)) {
            return false;
        }
    }
    return !seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, 25, SEEDTOOL_NAV_BACK);
}

/* The bar's red and green appear nowhere else on the screen, so both are
 * checked by counting pixels. */
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

/* Lit rows of the nav chrome, scanned band by band: text wraps rather than
 * clips, so a title one word too long lands in the band below. The bands are
 * seedtool_render.c's geometry, kept in step with the numbers here. */
/* The 1px rules are allowed in the gaps, by colour (COLOR_DIM); text is
 * brighter and fails. */
#define NAV_RULE_COLOR UINT16_C(0x39e7)

static bool nav_band_is_clear(const int from, const int to)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    for (int y = from; y < to; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] == NAV_RULE_COLOR) {
                continue;
            }
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000) {
                return false;
            }
        }
    }
    return true;
}

/* Nothing lit at or right of `x` in the title band: an overlong title runs off
 * the right, since its offset clamps at zero on the left. */
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

/* NAV_BAR_Y in seedtool_render.c, kept in sync with this: the band the confirm
 * bar used to occupy, still the floor the body must stay above. */
#define NAV_BAR_BAND_Y 118

/* The confirm's box in the title bar's right slot is drawn, inside the glass. */
static bool nav_right_slot_is_drawn(void)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    /* NAV_BACK_X/WIDTH/Y/HEIGHT in seedtool_render.c, mirrored to the right
     * edge, kept in sync with the numbers here. */
    const int left = SEEDTOOL_DISPLAY_WIDTH - 2 - 20;
    for (int y = 1; y < 1 + 19; ++y) {
        for (int x = left; x < left + 20; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000) {
                return x + 20 <= SEEDTOOL_DISPLAY_WIDTH;
            }
        }
    }
    return false;
}

/* The arrow's box is drawn: on a notice it is the only way out. */
static bool nav_left_slot_is_drawn(void)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    /* NAV_BACK_X = 2, NAV_BACK_WIDTH = 20, NAV_BACK_Y = 1, NAV_BACK_HEIGHT = 19
     * in seedtool_render.c, kept in sync with the numbers here. */
    for (int y = 1; y < 1 + 19; ++y) {
        for (int x = 2; x < 2 + 20; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000) {
                return true;
            }
        }
    }
    return false;
}

/* Walking a value with seedtool_render_fit_grouped must reproduce it character
 * for character, and each cut must land on a group boundary so the alternating
 * ink stays meaningful. */
/* A truncated row shows an ellipsis inside its own column. Comparing against
 * the prefix drawn as its own row proves the dots reach the screen at all. */
static bool truncated_rows_show_an_ellipsis(void)
{
    static uint16_t truncated[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];
    /* Longer than any row can hold, which is what every address row is: an
     * index, two spaces and a full address. */
    static const char* const row = "  0  bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
    const seedtool_nav_t nav = { .selected = 0, .back = true };
    const char* const items[] = { row };
    seedtool_render_nav_list(&nav, "m/84'/0'/0'/0", items, 1, 0);
    memcpy(truncated, seedtool_render_pixels(), sizeof(truncated));

    int rightmost = -1;
    for (int y = 21; y < 55; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (truncated[y * SEEDTOOL_DISPLAY_WIDTH + x] != 0x0000 && x > rightmost) {
                rightmost = x;
            }
        }
    }
    if (rightmost < 0) {
        return false; /* the row never reached the screen */
    }
    /* LIST_TEXT_X + LIST_TEXT_WIDTH in seedtool_render.c: the ellipsis stays in
     * the row's column. */
    if (rightmost > 6 + 222) {
        return false;
    }

    /* What a silent cut would have drawn. */
    char prefix[64] = { 0 };
    const size_t fit = seedtool_render_fit_row(row);
    if (fit >= sizeof(prefix) || fit >= strlen(row)) {
        return false; /* the row was meant to be too long for one */
    }
    memcpy(prefix, row, fit);
    const char* const cut[] = { prefix };
    seedtool_render_nav_list(&nav, "m/84'/0'/0'/0", cut, 1, 0);
    return memcmp(truncated, seedtool_render_pixels(), sizeof(truncated)) != 0;
}

static bool grouped_paging_preserves_the_value(void)
{
    static const char* const values[] = {
        /* Both address shapes this firmware derives, plus lengths that land
         * exactly on a group boundary and one past it. */
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
        "bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr",
        "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2",
        "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy",
        "abcd",
        "abcde",
        "abcdefgh",
    };
    for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
        const char* const value = values[v];
        const size_t total = strlen(value);
        char rebuilt[128] = { 0 };
        size_t used = 0, offset = 0, lines = 0;
        while (offset < total && lines < 32) {
            const size_t fit = seedtool_render_fit_grouped(value + offset, 48);
            if (!fit) {
                return false; /* no progress: the walk would never end */
            }
            /* A whole number of groups unless this is the value's own tail. */
            if (offset + fit < total && fit % SEEDTOOL_GROUP_LEN) {
                return false;
            }
            if (used + fit >= sizeof(rebuilt)) {
                return false;
            }
            memcpy(rebuilt + used, value + offset, fit);
            used += fit;
            offset += fit;
            ++lines;
        }
        rebuilt[used] = '\0';
        if (offset != total || strcmp(rebuilt, value) != 0) {
            return false;
        }
    }
    return true;
}

/* seedtool_render_qr_address must refuse an address that does not fit its
 * margin (taproot, 62 characters against 48) rather than cut it. Changing only
 * the last group must change the margin, which proves the tail is drawn; the
 * margin is everything left of the first tall run of white. */
static int qr_code_left_edge(void)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
        int run = 0;
        for (int y = 0; y < SEEDTOOL_DISPLAY_HEIGHT; ++y) {
            run = pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] == 0xffff ? run + 1 : 0;
            if (run > 40) {
                return x;
            }
        }
    }
    return -1;
}

static bool qr_address_draws_every_group(void)
{
    static const char* const fits[] = {
        "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",
        "1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2",
        "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy",
    };
    static uint16_t before[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];

    for (size_t v = 0; v < sizeof(fits) / sizeof(fits[0]); ++v) {
        const char* const value = fits[v];
        if (!seedtool_render_qr_address("m/84'/0'/0'/0/0", value, SEEDTOOL_NAV_BACK)) {
            return false; /* these do fit; refusing them is its own failure */
        }
        const int edge = qr_code_left_edge();
        if (edge <= 0) {
            return false;
        }
        memcpy(before, seedtool_render_pixels(), sizeof(before));

        /* The same value with its last group altered, same length so the code
         * keeps its version and the margin keeps its width. */
        char altered[80] = { 0 };
        const size_t total = strlen(value);
        if (total + 1 > sizeof(altered) || total < SEEDTOOL_GROUP_LEN) {
            return false;
        }
        memcpy(altered, value, total);
        for (size_t i = total - SEEDTOOL_GROUP_LEN; i < total; ++i) {
            altered[i] = altered[i] == 'q' ? 'p' : 'q';
        }
        if (!seedtool_render_qr_address("m/84'/0'/0'/0/0", altered, SEEDTOOL_NAV_BACK)) {
            return false;
        }
        const uint16_t* const after = seedtool_render_pixels();

        bool margin_changed = false;
        for (int y = 0; y < SEEDTOOL_DISPLAY_HEIGHT && !margin_changed; ++y) {
            for (int x = 0; x < edge; ++x) {
                if (before[y * SEEDTOOL_DISPLAY_WIDTH + x] != after[y * SEEDTOOL_DISPLAY_WIDTH + x]) {
                    margin_changed = true;
                    break;
                }
            }
        }
        if (!margin_changed) {
            return false; /* the tail was never drawn */
        }
    }

    /* And the one the margin cannot hold, which must say so rather than clip. */
    return !seedtool_render_qr_address("m/86'/0'/0'/0/0",
        "bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr", SEEDTOOL_NAV_BACK);
}

/* How far right the orange reaches in the strip above the margin's text, which
 * says which control the cursor is on. */
static int qr_highlight_reach(void)
{
    const uint16_t highlight = 0xfd20;
    const uint16_t* const pixels = seedtool_render_pixels();
    int reach = -1;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < SEEDTOOL_DISPLAY_WIDTH; ++x) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH + x] == highlight && x > reach) {
                reach = x;
            }
        }
    }
    return reach;
}

/* The cursor reaches the sun on every QR screen, animated and carousel
 * included. */
static bool qr_chrome_is_the_same_everywhere(void)
{
    static const char* const text = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
    static const uint8_t entropy[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    for (int screen = 0; screen < 5; ++screen) {
        int reach[2];
        for (int i = 0; i < 2; ++i) {
            const size_t selected = i ? SEEDTOOL_NAV_SHADE : SEEDTOOL_NAV_BACK;
            const bool ok = screen == 0 ? seedtool_render_qr("Account key", text, selected)
                : screen == 1           ? seedtool_render_qr_address("m/84'/0'/0'/0/0", text, selected)
                : screen == 2 ? seedtool_render_qr_bytes("Compact SeedQR", entropy, sizeof(entropy), selected)
                : screen == 3 ? seedtool_render_qr_bytes_map("Compact SeedQR", entropy, sizeof(entropy), selected)
                              : seedtool_render_qr_bytes_region("Compact SeedQR", entropy, sizeof(entropy), 0, selected);
            if (!ok) {
                return false;
            }
            reach[i] = qr_highlight_reach();
        }
        if (reach[0] < 0 || reach[1] <= reach[0]) {
            return false; /* no cursor, or one that cannot reach the sun */
        }
    }
    return true;
}

/* A shade step moves the light half of the code and nothing else, the ring
 * closes, and the cursor is visibly on the control. Measured off the panel, not
 * off the ramp. */
static bool qr_shade_moves_only_the_light_field(void)
{
    static const char* const value = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";
    static const char* const path = "m/84'/0'/0'/0/0";
    static uint16_t white[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];
    static uint16_t cycled[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];
    const size_t count = sizeof(white) / sizeof(white[0]);

    /* Nothing has touched the shade yet, so this is also the proof that a QR
     * left alone is drawn exactly as it was before the ramp existed. */
    if (!seedtool_render_qr_address(path, value, SEEDTOOL_NAV_BACK)) {
        return false;
    }
    memcpy(white, seedtool_render_pixels(), sizeof(white));

    /* The highlight must move onto the second control, not merely leave the
     * first. */
    const int back_reach = qr_highlight_reach();
    if (back_reach < 0) {
        return false; /* the cursor was never on the way out either */
    }
    if (!seedtool_render_qr_address(path, value, SEEDTOOL_NAV_SHADE)) {
        return false;
    }
    if (qr_highlight_reach() <= back_reach) {
        return false; /* the highlight did not move to the second control */
    }

    seedtool_render_qr_cycle_shade();
    if (!seedtool_render_qr_address(path, value, SEEDTOOL_NAV_BACK)) {
        return false;
    }
    memcpy(cycled, seedtool_render_pixels(), sizeof(cycled));

    /* Every pixel that moved was white and is now one single other colour -
     * the code's light field and the swatch on the button that reports it. */
    uint16_t shade = 0xffff;
    size_t moved = 0;
    for (size_t i = 0; i < count; ++i) {
        if (white[i] == cycled[i]) {
            continue;
        }
        if (white[i] != 0xffff) {
            return false; /* something other than the light field moved */
        }
        if (!moved) {
            shade = cycled[i];
        } else if (cycled[i] != shade) {
            return false; /* the light field is no longer one colour */
        }
        ++moved;
    }
    if (!moved || shade == 0xffff) {
        return false;
    }

    /* Round the ramp and back to white, each press changing something. */
    for (int i = 0; i < 32; ++i) {
        seedtool_render_qr_cycle_shade();
        if (!seedtool_render_qr_address(path, value, SEEDTOOL_NAV_BACK)) {
            return false;
        }
        if (memcmp(cycled, seedtool_render_pixels(), sizeof(cycled)) == 0) {
            return false; /* a press that changed nothing */
        }
        memcpy(cycled, seedtool_render_pixels(), sizeof(cycled));
        if (memcmp(white, cycled, sizeof(white)) == 0) {
            /* Home again, and the ramp is left where every other test expects
             * to find it. */
            return true;
        }
    }
    return false; /* the ring never came back to where it started */
}

/* The grouped line is centred with its gaps counted in, so it can run off an
 * edge in a way the plain centred line cannot. Measured, not assumed. */
static bool grouped_lines_clear_the_edges(void)
{
    static const char* const lines[] = { "bc1qw508d6qejxtdg4y5r3zarvary0", "bc1p5cyxnuxmeuwuvkwfem96lqzs" };
    static const size_t first_group[] = { 0, 7, 14 };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        /* The widest a line can be: fit_grouped's own answer for this value. */
        char line[64] = { 0 };
        const size_t fit = seedtool_render_fit_grouped(lines[i], 48);
        memcpy(line, lines[i], fit);
        const char* const shown[] = { line, line, line };
        const seedtool_nav_t nav = { .selected = SEEDTOOL_NAV_BODY,
            .confirm_enabled = true,
            .back = true,
            .counter = "1/2" };
        seedtool_render_nav_grouped(&nav, "m/84'/0'/0'/0/0", shown, first_group, 3);
        const uint16_t* const pixels = seedtool_render_pixels();
        for (int y = 30; y < 100; ++y) {
            if (pixels[y * SEEDTOOL_DISPLAY_WIDTH] != 0x0000
                || pixels[y * SEEDTOOL_DISPLAY_WIDTH + SEEDTOOL_DISPLAY_WIDTH - 1] != 0x0000) {
                return false;
            }
        }
    }
    return true;
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
        /* Every state the chrome is drawn in: the arrow lit, an item lit with
         * the confirm dimmed, and the confirm lit. */
        static const size_t selections[] = { SEEDTOOL_NAV_BACK, 0, SEEDTOOL_NAV_CONFIRM };
        for (size_t s = 0; s < sizeof(selections) / sizeof(selections[0]); ++s) {
            /* Item 0 is the invalid-checksum case, where the confirm is dimmed. */
            const bool confirm_enabled = selections[s] != 0;
            const seedtool_nav_t nav = { .selected = selections[s],
                .confirm = SEEDTOOL_CONFIRM_TICK,
                .confirm_enabled = confirm_enabled,
                .back = true, };
            seedtool_render_nav_list(&nav, titles[i], words, 12, 0);
            /* Title bar ends at 20 and rows run 21..116; the gaps must stay
             * dark. */
            if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(115, 118)) {
                return false;
            }
            if (!nav_right_slot_is_drawn()) {
                return false;
            }
            /* The title's column ends where the tick's box begins, so the title
             * is measured on a rendering without the tick. */
            const seedtool_nav_t bare = { .selected = nav.selected,
                .confirm_enabled = nav.confirm_enabled,
                .back = true };
            seedtool_render_nav_list(&bare, titles[i], words, 12, 0);
            if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
                return false;
            }
        }
    }
    /* A menu has no confirm: the right slot must stay empty. */
    for (size_t s = 0; s < 2; ++s) {
        const seedtool_nav_t nav
            = { .selected = s ? SEEDTOOL_NAV_BACK : 0, .back = true };
        seedtool_render_nav_list(&nav, "Word entry", words, 2, 0);
        if (!nav_band_is_clear(NAV_BAR_BAND_Y, SEEDTOOL_DISPLAY_HEIGHT) || nav_right_slot_is_drawn()) {
            return false;
        }
    }
    static const struct {
        const char* title;
        const char* line1;
        const char* line2;
        const char* label;
    } screens[] = {
        /* The widest text each screen builds, copied from seedtool_app.c. */
        { "Word 12/24", "mosquito", "Number 2048 of 2048", "Use this word" },
        { "Confirm backup", "Retype 8 of the 24 words", "Have your backup ready", "Start quiz" },
        { "Checksum valid", "BIP39 English", "Derivation unlocked", "Open wallet" },
        { "Word doesn't match", "Check your backup", NULL, "Try again" },
        { "Compact SeedQR", "Encodes your ENTIRE seed", "A photo = total loss of funds", "Show QR" },
        { "QR export", "Account key included", "A photo reveals every address", "Show QR" },
        { "Descriptor export", "Account key included", "A photo reveals every address", "Show descriptor" },
        { "Confirm passphrase", "Enter it a second time", "Exact match required", "Enter again" },
        /* The entropy verdicts have no title; the finding is in the body. */
        { NULL, "Poor entropy!", "128 of 128 bits", "Proceed anyway" },
        { NULL, "Pattern detected!", NULL, "Proceed anyway" },
        { "Flip word 23 again?", "2048  mosquito", "These flips are lost", "Flip again" },
    };
    for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); ++i) {
        for (int on_back = 0; on_back < 2; ++on_back) {
            const seedtool_nav_t nav = { .selected = on_back ? SEEDTOOL_NAV_BACK : SEEDTOOL_NAV_CONFIRM,
                .confirm = SEEDTOOL_CONFIRM_TICK,
                .confirm_enabled = true,
                .back = true, };
            seedtool_render_nav_text(&nav, screens[i].title, screens[i].line1, screens[i].line2, NULL);
            /* Line 2 sits at 65, so its wraps land at 81, 97 and 113; one wrap
             * is allowed, a second would reach the bottom. */
            if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(97, 118)) {
                return false;
            }
            /* The confirm must be drawn. */
            if (!nav_right_slot_is_drawn()) {
                return false;
            }
            /* Measured without the tick, as above. */
            const seedtool_nav_t bare = { .selected = nav.selected,
                .confirm_enabled = true,
                .back = true };
            seedtool_render_nav_text(&bare, screens[i].title, screens[i].line1, screens[i].line2, NULL);
            if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
                return false;
            }
        }
    }
    /* With the entropy bar (y=90..103), the gap below it must stay clear. */
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
            const seedtool_nav_t nav = { .selected = on_back ? SEEDTOOL_NAV_BACK : SEEDTOOL_NAV_CONFIRM,
                .confirm = SEEDTOOL_CONFIRM_TICK,
                .confirm_enabled = true,
                .back = true, };
            seedtool_render_nav_dice(&nav, dice[i].title, dice[i].line1, dice[i].line2, &full);
            if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(104, 118)) {
                return false;
            }
            if (!nav_right_slot_is_drawn()) {
                return false;
            }
            /* Title measured tick-free, for the reason given above: the box
             * stands exactly where an overlong title would land. */
            const seedtool_nav_t bare = { .selected = nav.selected,
                .confirm_enabled = true,
                .back = true };
            seedtool_render_nav_dice(&bare, dice[i].title, dice[i].line1, dice[i].line2, &full);
            if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
                return false;
            }
        }
    }
    /* Notices: an arrow and no tick, the title held to the same column. */
    static const struct {
        const char* title;
        const char* line1;
        const char* line2;
    } notices[] = {
        { "Passphrase mismatch", "Nothing was derived", "Try again" },
        { "Backup confirmed", "Words matched", NULL },
        { "Too long for a QR", "Compact SeedQR", "Read it as text instead" },
        { "Error", "Could not derive addresses", NULL },
        { "Error", "Could not compute", "word numbers" },
    };
    for (size_t i = 0; i < sizeof(notices) / sizeof(notices[0]); ++i) {
        const seedtool_nav_t nav = { .selected = SEEDTOOL_NAV_BACK,
            .confirm_enabled = true,
            .back = true };
        seedtool_render_nav_text(&nav, notices[i].title, notices[i].line1, notices[i].line2, NULL);
        /* The arrow must be drawn and the tick absent. */
        if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(97, NAV_BAR_BAND_Y) || !nav_left_slot_is_drawn()
            || nav_right_slot_is_drawn()) {
            return false;
        }
    }
    /* The paged screens: the body ends at 103 and the page counter sits below,
     * so 116..118 must stay clear. */
    static const size_t cursors[] = { SEEDTOOL_NAV_BACK, 0, SEEDTOOL_NAV_CONFIRM };
    for (size_t c = 0; c < sizeof(cursors) / sizeof(cursors[0]); ++c) {
        /* A full-width transcript line, and the widest counter either paged
         * screen can reach: MAX_PAGE_LINES=24 gives page_text eight pages. */
        const seedtool_nav_t paged = { .selected = cursors[c],
            .confirm = SEEDTOOL_CONFIRM_TICK,
            .confirm_enabled = true,
            .back = true,
            .counter = "8/8", };
        seedtool_render_nav_text(&paged, "Canonical transcript", "20-1-2-1-4-1-1-1-1-1-1-1-1-",
            "1-1-1-1-1-1-1-1-1-20-1-1-1-", "1-1-1-1-1-1-1-1-1-1");
        if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(116, 118)) {
            return false;
        }
        if (!nav_right_slot_is_drawn()) {
            return false;
        }
        /* Title measured tick-free, for the reason the list pass above gives. */
        const seedtool_nav_t bare_paged
            = { .selected = paged.selected, .confirm_enabled = true, .back = true,
                  .counter = paged.counter };
        seedtool_render_nav_text(&bare_paged, "Canonical transcript", "20-1-2-1-4-1-1-1-1-1-1-1-1-",
            "1-1-1-1-1-1-1-1-1-20-1-1-1-", "1-1-1-1-1-1-1-1-1-1");
        if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
            return false;
        }
        static const char* const rows[] = { "21. mosquito", "22. mosquito", "23. mosquito", "24. mosquito" };
        const seedtool_nav_t listed = { .selected = cursors[c],
            .confirm = SEEDTOOL_CONFIRM_TICK,
            .confirm_enabled = true,
            .back = true,
            .counter = "6/6", };
        seedtool_render_nav_rows(&listed, "BIP39 word numbers", rows, 4);
        if (!nav_band_is_clear(20, 21) || !nav_band_is_clear(116, 118)) {
            return false;
        }
        if (!nav_right_slot_is_drawn()) {
            return false;
        }
        /* Title measured tick-free, as above. */
        const seedtool_nav_t bare_rows = { .selected = listed.selected,
            .confirm_enabled = true, .back = true, .counter = listed.counter };
        seedtool_render_nav_rows(&bare_rows, "BIP39 word numbers", rows, 4);
        if (!nav_title_stays_in_its_column(SEEDTOOL_DISPLAY_WIDTH - (2 + 20))) {
            return false;
        }
    }
    return true;
}

/* Inverse of seedtool_bbqr_base32_encode, for the round trip below. */
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

/* An account key split into BBQr parts must reassemble byte for byte, with the
 * right "B$2U" header, total and index on every part. */
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

/* Every value the QR screen offers must encode. The account key payload is
 * the longest the device ever shows, and with a three-digit account it lands
 * right at the encoder's 134-byte ceiling. */
static bool account_key_qr_fits(void)
{
    for (size_t i = 0; i < 2; ++i) {
        char payload[160];
        const unsigned purpose = i ? 86 : 84;
        (void)snprintf(payload, sizeof(payload), "[73c5da0a/%u'/0'/%u']%s", purpose, SEEDTOOL_MAX_ACCOUNT_INDEX,
            i ? expected_xpub86 : expected_xpub84);
        CHECK(strlen(payload) <= 134);
        CHECK(seedtool_render_qr("BIP84 account key", payload, SEEDTOOL_NAV_BACK));
    }
    return true;
}

static bool vectors_page_losslessly(void)
{
    CHECK(paging_is_lossless(expected_xpub84));
    CHECK(paging_is_lossless(expected_xpub86));
    CHECK(paging_is_lossless(expected_address));
    CHECK(paging_is_lossless(mnemonic));
    CHECK(paging_is_lossless("cards-v1:ACKS7D2H"));
    return true;
}

/* Draws what the other tests do not reach, so the sanitizers see every
 * renderer run: the plain screen, the splash, a scrolled list and each real
 * keyboard layout opened at its centre. */
static bool remaining_screens_render(void)
{
    static const char* const rows[] = { "Master fingerprint", "Native SegWit", "Taproot", "Backup", "Settings",
        "About", "Erase and restart" };
    seedtool_render_screen("ORIGO", "HOST SELF-TEST", expected_address, "OK");
    seedtool_render_splash();
    const seedtool_nav_t scrolled = { .selected = 6, .confirm_enabled = false, .back = true };
    const seedtool_nav_t at_start = { .selected = 0, .confirm_enabled = false, .back = true };
    seedtool_render_nav_list(&scrolled, "Wallet", rows, 7, 4);
    seedtool_render_nav_list(&at_start, "Word 3/12  aba", rows, 7, 0);
    const bool letters[SEEDTOOL_LETTERS + 1] = { true };
    seedtool_render_keyboard(
        "Word 1/12", "aba", SEEDTOOL_WORD_LAYOUT, letters, seedtool_layout_center(SEEDTOOL_WORD_LAYOUT), 1, 12);
    seedtool_render_keyboard("Word 12/12", "4", SEEDTOOL_WORD_NUMBER_LAYOUT, NULL,
        seedtool_layout_center(SEEDTOOL_WORD_NUMBER_LAYOUT), 12, 12);
    seedtool_render_keyboard("Word 1/12", "aba", SEEDTOOL_WORD_LAYOUT_ABC, letters,
        seedtool_layout_center(SEEDTOOL_WORD_LAYOUT_ABC), 1, 12);
    for (size_t page = 0; page < SEEDTOOL_PASSPHRASE_PAGES; ++page) {
        seedtool_render_keyboard("BIP39 passphrase", "", seedtool_passphrase_layouts[page], NULL,
            seedtool_layout_center(seedtool_passphrase_layouts[page]), 0, 0);
        seedtool_render_keyboard("BIP39 passphrase", "", seedtool_passphrase_layouts_abc[page], NULL,
            seedtool_layout_center(seedtool_passphrase_layouts_abc[page]), 0, 0);
    }
    return true;
}

int main(void)
{
    if (wally_init(0) != WALLY_OK) {
        return 1;
    }
    static const test_t tests[] = {
        TEST(wire_order_is_big_endian),
        TEST(layouts_are_complete),
        TEST(list_viewport_is_sound),
        TEST(partial_transcripts_are_prefixes),
        TEST(stackbit_grid_is_sound),
        TEST(stackbit_physical_grid_is_sound),
        TEST(compact_seedqr_is_sound),
        TEST(zoomed_qr_regions_are_sound),
        TEST(dice_progress_bar_is_bounded),
        TEST(truncated_rows_show_an_ellipsis),
        TEST(grouped_paging_preserves_the_value),
        TEST(qr_address_draws_every_group),
        TEST(qr_chrome_is_the_same_everywhere),
        TEST(qr_shade_moves_only_the_light_field),
        TEST(grouped_lines_clear_the_edges),
        TEST(nav_chrome_bands_do_not_collide),
        TEST(bbqr_parts_are_sound),
        TEST(account_key_qr_fits),
        TEST(vectors_page_losslessly),
        TEST(remaining_screens_render),
    };
    const int ret = run_tests("render", tests, sizeof(tests) / sizeof(tests[0]));
    wally_cleanup(0);
    return ret;
}
