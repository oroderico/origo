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

/* `layout` is the key characters row by row, rows separated by '\n' and at most
 * ten keys per row. `enabled` is indexed by key position across the whole
 * layout, ignoring the separators; disabled keys are drawn dimmed and are meant
 * to be unreachable. `selected` is a key position and is drawn highlighted. */
void seedtool_render_keyboard(
    const char* title, const char* text, const char* layout, const bool* enabled, size_t selected);

/* Layout geometry, kept here so the string is read the same way everywhere it is
 * drawn, walked or measured. */
size_t seedtool_layout_keys(const char* layout);
char seedtool_layout_key(const char* layout, size_t index);

/* Key position at the visual centre: the middle key of the middle row. It is
 * where a keyboard opens, because with two buttons on a ring of thirty keys the
 * corner is the furthest possible place to start from. */
size_t seedtool_layout_center(const char* layout);

bool seedtool_render_qr(const char* title, const char* text);

/* Same as seedtool_render_qr, but for raw bytes rather than a null-terminated
 * string: entropy can contain embedded 0x00 bytes, which qrcode_initText's
 * strlen() would silently truncate at. `len` is passed straight through to
 * the byte-mode encoder instead. */
bool seedtool_render_qr_bytes(const char* title, const uint8_t* data, size_t len);

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
