#ifndef SEEDTOOL_RENDER_H_
#define SEEDTOOL_RENDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_DISPLAY_WIDTH 240
#define SEEDTOOL_DISPLAY_HEIGHT 135

/* Rows a list shows at once. Every choice in the firmware is made through a list
 * of this height, so an option is always read with its neighbours in view. Three
 * rows rather than five buys each one the 16px face instead of the 11px one:
 * this is read at arm's length off a screen an inch across. */
#define SEEDTOOL_LIST_ROWS 3

/* Keyboard control keys. They occupy one cell of the layout like any other key
 * but are drawn as a short word instead of a glyph. */
#define SEEDTOOL_KEY_BACKSPACE '\b'
#define SEEDTOOL_KEY_PAGE '\t'
#define SEEDTOOL_KEY_ACCEPT '\r'

void seedtool_render_clear(void);
void seedtool_render_screen(const char* title, const char* line1, const char* line2, const char* footer);

/* Same as seedtool_render_screen, but with a third body line: for a long
 * value paged three lines at a time instead of two. */
void seedtool_render_screen3(
    const char* title, const char* line1, const char* line2, const char* line3, const char* footer);

/* Same again, with a fourth body line: for a numbered word or word-number
 * list, one entry per line, four entries per page. */
void seedtool_render_screen4(const char* title, const char* line1, const char* line2, const char* line3,
    const char* line4, const char* footer);

/* The dice-roll quality bar drawn under a D6/D20 entry screen: a top segment
 * for rolls collected against the minimum, a bottom segment for Shannon's
 * entropy of those rolls against the minimum bits needed. `warn` colors the
 * entropy segment as a pattern warning; `complete` colors the outline once
 * both minimums are met. Adapted from Krux's dice-roll entropy screen
 * (github.com/selfcustody/krux, src/krux/pages/new_mnemonic/dice_rolls.py). */
typedef struct {
    int rolls_pct; /* 0-100 */
    int entropy_pct; /* 0-100 */
    bool warn;
    bool complete;
    /* The count behind entropy_pct, in bits rather than percent, for a screen
     * that prints the number rather than only filling the bar. `graded` says
     * whether there is one: the default struct literal leaves it false, which
     * is what the screens that draw a bar without a running estimate - and the
     * checksum-completion flips, which are not graded at all - rely on. It is
     * a flag rather than a sentinel value of `bits`, because zero bits is a
     * real and meaningful reading at the start of every run. */
    bool graded;
    int bits;
} seedtool_progress_t;

/* Draws the same screen as seedtool_render_screen, then the bar on top of it
 * in the gap between line2 and the footer, when `progress` is not NULL. */
void seedtool_render_dice_screen(
    const char* title, const char* line1, const char* line2, const char* footer, const seedtool_progress_t* progress);

/* The opening screen: the logo, centred, and nothing else. */
void seedtool_render_splash(void);

/* How many leading characters of `text` fit on one body line, up to `limit`.
 * The body font is proportional, so callers must split by this rather than by a
 * fixed character count: an address silently wrapped onto the next row would be
 * transcribed wrongly. */
size_t seedtool_render_fit(const char* text, size_t limit);

/* How many leading characters of a list item are drawn before the row runs out
 * of width. A label that does not fit is cut here, so this is what a caller
 * checks a new label against. Values meant to be transcribed never go in a list:
 * they are paged with `seedtool_render_fit` instead, which loses nothing. */
size_t seedtool_render_fit_row(const char* text);

/* How many *trailing* characters of `text` fit one body line. A running
 * transcript is read from its end, where what was just entered is. */
size_t seedtool_render_fit_tail(const char* text);

/* First visible row so that `selected` is on screen, scrolling as little as
 * possible away from `previous_top`. A pure function of its arguments: the
 * entry path is exactly where the device RNG must not reach, and where the list
 * has scrolled to is part of that path. */
size_t seedtool_list_top(size_t count, size_t selected, size_t previous_top);

/* Scrollbar thumb inside a track of `track` pixels: `height` is how much of the
 * list is on screen, `offset` is how far into it. Pure, like the scroll position
 * itself, so the self-test can prove the thumb never leaves its track. */
typedef struct {
    int offset;
    int height;
} seedtool_thumb_t;

seedtool_thumb_t seedtool_list_thumb(size_t count, size_t top, int track);

void seedtool_render_list(const char* title, const char* const* items, size_t count, size_t selected, size_t top,
    const char* footer);

/* The two fixed controls a nav screen always carries, addressed as selections
 * alongside an ordinary item index. Sentinels rather than positions past the
 * end of the list, so a caller never has to know how many rows the chrome
 * added to reach them - "the back arrow" and "the confirm bar" name
 * themselves. SEEDTOOL_NAV_BODY is neither: it says the cursor is on what the
 * screen is showing, which is how a paged screen says "reading, not on a
 * control". Adapted from Blockstream Jade, which puts a back button in every
 * title bar (main/ui/dialogs.c) rather than spending a list row on it. */
#define SEEDTOOL_NAV_BACK SIZE_MAX
#define SEEDTOOL_NAV_CONFIRM (SIZE_MAX - 1)
#define SEEDTOOL_NAV_BODY (SIZE_MAX - 2)

/* The chrome a nav screen wears: a back arrow at the top left, in the title
 * bar where it costs no row, and a confirm bar along the bottom. Both sit in
 * the same place on every screen that uses this, so neither button carries a
 * meaning that changes with the screen - up and down only ever move the
 * cursor, and the chord only ever takes what is highlighted.
 *
 * Passed as one struct rather than four parameters so that a screen names its
 * controls once, and so a screen type added later inherits them without every
 * renderer growing a signature. Every field is meant to be set explicitly:
 * the zero value of `confirm_enabled` dims a bar and the zero value of `back`
 * removes an arrow, neither of which is what a screen usually wants. The app
 * builds these in its own few nav helpers, not at each call site. */
typedef struct {
    /* An item index, or one of the three sentinels above. */
    size_t selected;
    /* Label on the confirm bar. NULL draws no bar at all: a menu's rows are
     * its actions, and there is nothing left to confirm once one is picked. */
    const char* confirm;
    /* False draws the bar dimmed and unselectable - for a screen where
     * confirming is not yet possible and the reader should still see where
     * the control will be. */
    bool confirm_enabled;
    /* False draws no arrow, for a notice with nowhere to go back to. Drawing
     * one there would restate as a control the same false promise the old
     * "Up/Down back" footer was making. */
    bool back;
    /* Page counter, drawn small in the gap above the bar. NULL draws none. */
    const char* counter;
} seedtool_nav_t;

/* Two or three centred body lines - three when `line3` is given, the same
 * choice seedtool_render_screen and seedtool_render_screen3 make by being two
 * functions. Body heights match theirs, so a screen gaining the chrome does
 * not also move its own text. */
void seedtool_render_nav_text(
    const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2, const char* line3);

/* Four left-aligned rows, at seedtool_render_screen4's own heights: a numbered
 * word list, which reads as a table rather than as centred blocks. */
void seedtool_render_nav_rows(const seedtool_nav_t* nav, const char* title, const char* const* rows, size_t count);

/* A scrolling list under the chrome. Shows the same SEEDTOOL_LIST_ROWS rows as
 * seedtool_render_list, one pixel shorter each to buy the bar its height, so
 * seedtool_list_top and seedtool_list_thumb apply here unchanged. */
void seedtool_render_nav_list(
    const seedtool_nav_t* nav, const char* title, const char* const* items, size_t count, size_t top);

/* Two body lines with the dice/coin quality bar drawn in, for the screens that
 * bracket an entropy run: the one before it showing the bar empty, and the one
 * after showing it full. */
void seedtool_render_nav_dice(const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2,
    const seedtool_progress_t* progress);

/* `layout` is the key characters row by row, rows separated by '\n' and at most
 * ten keys per row. `enabled` is indexed by key position across the whole
 * layout, ignoring the separators; disabled keys are drawn dimmed and are meant
 * to be unreachable. `selected` is a key position and is drawn highlighted.
 * `total` is the word count a mnemonic entry is part of; `position` (1-based)
 * is which word this screen is typing. A thin bar between the typed text and
 * the keys fills to `position - 1` of `total`, the words already behind this
 * one - passphrase and account-index entry have no such count, so `total` of
 * 0 skips the bar entirely rather than drawing an empty one. */
void seedtool_render_keyboard(const char* title, const char* text, const char* layout, const bool* enabled,
    size_t selected, size_t position, size_t total);

/* Layout geometry, kept here so the string is read the same way everywhere it is
 * drawn, walked or measured. */
size_t seedtool_layout_keys(const char* layout);
char seedtool_layout_key(const char* layout, size_t index);

/* Key position at the visual centre: the middle key of the middle row. It is
 * where a keyboard opens, because with two buttons on a ring of thirty keys the
 * corner is the furthest possible place to start from. */
size_t seedtool_layout_center(const char* layout);

bool seedtool_render_qr(const char* title, const char* text);

/* How many alphanumeric-mode characters (see qrcode_versionForAlphanumeric)
 * fit in one frame at `max_version` and ECC_LOW -- the same error-correction
 * level every other QR this firmware draws already commits to. What a
 * multi-part QR protocol such as BBQr needs to know to decide how many
 * frames a payload needs; capping `max_version` below this file's QR_VERSION
 * trades more, smaller frames for a coarser, more legible module grid on
 * each one. */
size_t seedtool_render_qr_alphanumeric_capacity(uint8_t max_version);

/* Same as seedtool_render_qr, but for raw bytes rather than a null-terminated
 * string: entropy can contain embedded 0x00 bytes, which qrcode_initText's
 * strlen() would silently truncate at. `len` is passed straight through to
 * the byte-mode encoder instead. */
bool seedtool_render_qr_bytes(const char* title, const uint8_t* data, size_t len);

/* How many "Zoomed Region" tiles seedtool_render_qr_bytes_region below can
 * step through for a byte-mode payload of `len` bytes: the QR is split into
 * 7x7-module blocks if it is the smallest (version 1, 21x21) QR, 5x5 blocks
 * otherwise (Krux's own thresholds, github.com/selfcustody/krux,
 * src/krux/pages/qr_view.py), covering it exactly since Compact SeedQR's two
 * possible sizes (21, 25) both divide evenly by their block size. Returns 0
 * on the same failure seedtool_render_qr_bytes_region would have. A pure
 * function of `len` alone, like seedtool_render_fit and friends: it does not
 * touch the framebuffer, so callers reach it directly rather than through
 * seedtool_display. */
size_t seedtool_render_qr_bytes_regions(size_t len);

/* The region map: the same full-size code seedtool_render_qr_bytes draws,
 * with a dim line at every region boundary and each region's own "A1" etc
 * label stamped inside its own block - the same split
 * seedtool_render_qr_bytes_regions counts, and the same dim-line-over-
 * modules convention a zoomed tile already uses on itself, just a region
 * apart instead of a module apart. Meant to be shown between the plain full
 * code and the zoomed tiles, so a reader has already seen the code at this
 * size before the labels are laid over it, and knows where "A1" sits
 * relative to "B2" before copying either onto a paper template. */
bool seedtool_render_qr_bytes_map(const char* title, const uint8_t* data, size_t len);

/* Draws one zoomed-in region tile of a byte-mode payload's QR code, in raster
 * order (region_index 0 is the top-left block, stepping right then down),
 * enlarged to nearly fill the display with a grid overlay and a "Region: A1"
 * legend, so it can be hand-copied onto a paper template too small to fit the
 * whole code drawn small enough to trace by eye. Krux's "Zoomed Regions Mode"
 * ported to this display's layout. `region_index` must be less than what
 * seedtool_render_qr_bytes_regions(len) returns. */
bool seedtool_render_qr_bytes_region(const char* title, const uint8_t* data, size_t len, size_t region_index);

/* The Stackbit 1248 punch-grid backup display: one word's one-based word
 * number (1-2048, the convention enter_word_number() restores by) shown as
 * four decimal digits, each digit a column of four punch cells for binary
 * weights 1, 2, 4 and 8 read top to bottom, lit where that digit's bits are
 * set. `word` is shown beside the grid so the number can be checked against
 * the word it means before it is punched. One simple layout, adapted from
 * Krux's Stackbit 1248 export (github.com/selfcustody/krux,
 * src/krux/pages/stack_1248.py) rather than its three alternates. */
void seedtool_render_stackbit_screen(const char* title, unsigned word_number, const char* word, const char* footer);

/* The same backup, laid out the way a physical Stackbit 1248 plate actually
 * is: two rows, not four, with the thousands digit a single column of two
 * cells and the other three digits each a 2x2 block. For anyone punching a
 * plate side by side with the screen and matching its layout by eye rather
 * than by the weight label alone. */
void seedtool_render_stackbit_physical_screen(
    const char* title, unsigned word_number, const char* word, const char* footer);

const uint16_t* seedtool_render_pixels(void);

/* Copies `rows` rows of the framebuffer into `out` in the order the panel reads
 * them, most significant byte of each pixel first. The framebuffer is
 * little-endian because the CPU is; the wire is not. Black and white are the
 * same in both orders, which is why a missing swap is invisible on a screen made
 * only of text — and was, until the logo. */
void seedtool_render_wire_rows(uint16_t* out, size_t first_row, size_t rows);

#endif
