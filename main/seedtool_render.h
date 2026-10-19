#ifndef SEEDTOOL_RENDER_H_
#define SEEDTOOL_RENDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_DISPLAY_WIDTH 240
#define SEEDTOOL_DISPLAY_HEIGHT 135

/* Rows a list shows at once: three, so each gets the 16px face, since the
 * screen is an inch across and read at arm's length. */
#define SEEDTOOL_LIST_ROWS 3

/* Keyboard control keys. They occupy one cell of the layout like any other key
 * but are drawn as a short word instead of a glyph. */
#define SEEDTOOL_KEY_BACKSPACE '\b'
#define SEEDTOOL_KEY_PAGE '\t'
#define SEEDTOOL_KEY_ACCEPT '\r'

void seedtool_render_clear(void);
void seedtool_render_screen(const char* title, const char* line1, const char* line2, const char* footer);

/* The entropy quality bar under an entry screen: rolls against the minimum on
 * top, Shannon entropy against the bits needed below. `warn` marks a pattern;
 * `complete` colours the outline once both minimums are met. Adapted from Krux
 * (src/krux/pages/new_mnemonic/dice_rolls.py). */
typedef struct {
    int rolls_pct; /* 0-100 */
    int entropy_pct; /* 0-100 */
    bool warn;
    bool complete;
    /* entropy_pct as bits, for a screen that prints the number. `graded` says
     * there is one: zero bits is a real reading at the start of a run, so it
     * cannot be the sentinel. */
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
 * The font is proportional, so values are split by this rather than by a
 * character count. */
size_t seedtool_render_fit(const char* text, size_t limit);

/* How many leading characters of a list item fit its row. Values meant to be
 * transcribed are never listed; they are paged with seedtool_render_fit. */
size_t seedtool_render_fit_row(const char* text);

/* Characters per group, as wallets group addresses. */
#define SEEDTOOL_GROUP_LEN 4

/* How many characters of `text` fit one line drawn grouped: fewer than
 * seedtool_render_fit, since the gaps take width, and a whole number of groups
 * unless the value ends mid-group. */
size_t seedtool_render_fit_grouped(const char* text, size_t limit);

/* How many *trailing* characters of `text` fit one body line. A running
 * transcript is read from its end, where what was just entered is. */
size_t seedtool_render_fit_tail(const char* text);

/* First visible row that keeps `selected` on screen, scrolling as little as
 * possible from `previous_top`. */
size_t seedtool_list_top(size_t count, size_t selected, size_t previous_top);

/* Scrollbar thumb inside a track of `track` pixels. */
typedef struct {
    int offset;
    int height;
} seedtool_thumb_t;

seedtool_thumb_t seedtool_list_thumb(size_t count, size_t top, int track);

/* The chrome's fixed controls, addressed as selections alongside an item index.
 * SEEDTOOL_NAV_BODY is the cursor on what the screen shows, as on a paged
 * screen. Adapted from Jade, which puts back in the title bar
 * (main/ui/dialogs.c). */
#define SEEDTOOL_NAV_BACK SIZE_MAX
#define SEEDTOOL_NAV_CONFIRM (SIZE_MAX - 1)
#define SEEDTOOL_NAV_BODY (SIZE_MAX - 2)
/* The QR's second control, which cycles how bright its light half is. */
#define SEEDTOOL_NAV_SHADE (SIZE_MAX - 3)

/* The chrome a nav screen wears: the back arrow at the top left of the title
 * bar and the confirm at the top right, in the same place on every screen, so
 * up and down only move the cursor and the chord only takes what is lit. Every
 * field is meant to be set: the zero value of `confirm_enabled` dims the
 * confirm and that of `back` removes the arrow. */
/* The confirm control in the title bar's right slot, opposite the back arrow
 * (Jade's ui/confirm_address.c layout). A tick accepts what the title names; a
 * forward arrow hands off to another screen without accepting anything. NONE,
 * the zero value, is a menu: its rows are its actions. */
typedef enum {
    SEEDTOOL_CONFIRM_NONE,
    SEEDTOOL_CONFIRM_TICK,
    SEEDTOOL_CONFIRM_FORWARD,
} seedtool_confirm_t;

typedef struct {
    /* An item index, or one of the three sentinels above. */
    size_t selected;
    seedtool_confirm_t confirm;
    /* False draws the confirm dimmed and unselectable, for a screen where it is
     * not possible yet. */
    bool confirm_enabled;
    /* False draws no arrow, for a notice with nowhere to go back to. */
    bool back;
    /* Page counter, drawn small at the bottom. NULL draws none. */
    const char* counter;
} seedtool_nav_t;

/* Two or three centred body lines; three when `line3` is given, at a tighter
 * pitch. */
void seedtool_render_nav_text(
    const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2, const char* line3);

/* The same chrome over a value drawn in groups of four, alternating white and
 * orange with a gap at each boundary, so a reader transcribing an address keeps
 * their place. `first_group` gives how many groups precede each line, so the
 * alternation carries across line breaks. Grouping happens only at draw time. */
void seedtool_render_nav_grouped(const seedtool_nav_t* nav, const char* title, const char* const* lines,
    const size_t* first_group, size_t count);

/* Four left-aligned rows: a numbered word list, which reads as a table rather
 * than as centred blocks. */
void seedtool_render_nav_rows(const seedtool_nav_t* nav, const char* title, const char* const* rows, size_t count);

/* A scrolling list under the chrome, SEEDTOOL_LIST_ROWS rows at a time. */
void seedtool_render_nav_list(
    const seedtool_nav_t* nav, const char* title, const char* const* items, size_t count, size_t top);

/* Two body lines with the entropy bar, for the screens before and after an
 * entropy run. */
void seedtool_render_nav_dice(const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2,
    const seedtool_progress_t* progress);

/* `enabled` is indexed by key position across the whole layout; disabled keys
 * are dimmed and meant to be unreachable. `selected` is the highlighted key.
 * `position` (1-based) of `total` fills a bar with the words already typed; a
 * `total` of 0 draws no bar. */
/* A row of digit boxes, the one at `active` carrying up and down arrows. Boxes
 * before `active` are set, those after it still to come; a
 * SEEDTOOL_KEY_BACKSPACE at `active` draws the backspace glyph. `progress`
 * draws the entropy bar when not NULL. `count` must not exceed
 * SEEDTOOL_DIGIT_BOXES_MAX, or the centred row runs off both edges. */
#define SEEDTOOL_DIGIT_BOXES_MAX 4
void seedtool_render_digits(const char* title, const char* digits, size_t count, size_t active, const char* footer,
    const seedtool_progress_t* progress);

/* One wider box for a value scrolled whole (a die face, a D20 roll) rather
 * than digit by digit. `back` draws the backspace glyph in place of `text`. */
void seedtool_render_value_box(
    const char* title, const char* text, bool back, const char* footer, const seedtool_progress_t* progress);

void seedtool_render_keyboard(const char* title, const char* text, const char* layout, const bool* enabled,
    size_t selected, size_t position, size_t total);

/* Keyboard layouts: the key characters row by row, rows separated by '\n' and at
 * most ten keys per row. Public so the tests can prove they cover what they
 * must: a layout that dropped a character would leave a passphrase impossible
 * to retype. */
#define SEEDTOOL_WORD_LAYOUT "qwertyuiop\nasdfghjkl\nzxcvbnm\b"
/* The same keys in alphabetical rows, as on Jade (main/ui/keyboard.c): on a
 * ring walked with two buttons, a reader who does not touch-type finds a
 * letter by where it falls in the alphabet. */
#define SEEDTOOL_WORD_LAYOUT_ABC "abcdefghij\nklmnopqrst\nuvwxyz\b"
/* Accept before Backspace: the cursor snaps to whichever is nearer on the ring,
 * and a completed number should land on Accept. */
#define SEEDTOOL_WORD_NUMBER_LAYOUT "1234567890\n\r\b"
#define SEEDTOOL_PASSPHRASE_PAGES 4
extern const char* const seedtool_passphrase_layouts[SEEDTOOL_PASSPHRASE_PAGES];
extern const char* const seedtool_passphrase_layouts_abc[SEEDTOOL_PASSPHRASE_PAGES];

/* Layout geometry, kept here so the string is read the same way everywhere it is
 * drawn, walked or measured. */
size_t seedtool_layout_keys(const char* layout);
char seedtool_layout_key(const char* layout, size_t index);

/* Key position at the visual centre, where a keyboard opens: the middle key of
 * the middle row. */
size_t seedtool_layout_center(const char* layout);

/* Cycles the shade of the light half of every QR, darker each step and back to
 * white: some cameras stop down on a white block until the code no longer
 * resolves. The dark modules never change. */
void seedtool_render_qr_cycle_shade(void);

bool seedtool_render_qr(const char* title, const char* text, size_t selected);

/* The code with its derivation path and the value beside it, for an address.
 * `selected` is SEEDTOOL_NAV_BACK or SEEDTOOL_NAV_SHADE. */
bool seedtool_render_qr_address(const char* title, const char* text, size_t selected);

/* Alphanumeric-mode characters one frame holds at `max_version` and ECC_LOW,
 * which BBQr needs to split a payload into frames. */
size_t seedtool_render_qr_alphanumeric_capacity(uint8_t max_version);

/* seedtool_render_qr for raw bytes, which can contain 0x00. */
bool seedtool_render_qr_bytes(const char* title, const uint8_t* data, size_t len, size_t selected);

/* How many zoomed tiles seedtool_render_qr_bytes_region steps through for `len`
 * bytes: 7x7-module blocks on a version 1 code, 5x5 otherwise (Krux,
 * src/krux/pages/qr_view.py), which divide both Compact SeedQR sizes exactly. 0
 * on the same failure the tile would have. */
size_t seedtool_render_qr_bytes_regions(size_t len);

/* The full code with a dim line at every region boundary and each region's
 * label ("A1") inside it, shown before the zoomed tiles so the reader knows
 * where each sits. */
bool seedtool_render_qr_bytes_map(const char* title, const uint8_t* data, size_t len, size_t selected);

/* One region of the code enlarged to nearly fill the display, with a grid and a
 * "Region: A1" legend, for copying onto a paper template. Regions go in raster
 * order. Krux's "Zoomed Regions" mode. `region_index` must be below
 * seedtool_render_qr_bytes_regions(len). */
bool seedtool_render_qr_bytes_region(
    const char* title, const uint8_t* data, size_t len, size_t region_index, size_t selected);

/* A Stackbit 1248 punch grid: the word's one-based number as four digits, each
 * a column of cells weighted 1, 2, 4 and 8, lit where its bits are set, with
 * the word beside it. Adapted from Krux (src/krux/pages/stack_1248.py). */
void seedtool_render_stackbit_screen(
    const seedtool_nav_t* nav, const char* title, unsigned word_number, const char* word, const char* footer);

/* The same, laid out as the physical plate: two rows, the thousands digit a
 * column of two cells and the others 2x2 blocks. */
void seedtool_render_stackbit_physical_screen(
    const seedtool_nav_t* nav, const char* title, unsigned word_number, const char* word, const char* footer);

const uint16_t* seedtool_render_pixels(void);

/* Copies `rows` rows of the framebuffer into `out` in the panel's byte order,
 * most significant byte first; the framebuffer is little-endian like the CPU. */
void seedtool_render_wire_rows(uint16_t* out, size_t first_row, size_t rows);

#endif
