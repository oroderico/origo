#include "seedtool_render.h"

#include "qrcode.h"
#include "seedtool_logo.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define COLOR_BLACK UINT16_C(0x0000)
#define COLOR_WHITE UINT16_C(0xffff)
#define COLOR_DIM UINT16_C(0x39e7)
#define COLOR_HIGHLIGHT UINT16_C(0xfd20)
#define COLOR_WARN UINT16_C(0xf800)
#define COLOR_GO UINT16_C(0x07e0)

/* Five greys for the QR's light half, near-black to white (16, 74, 124, 198,
 * 255 as RGB565). The ramp starts at white. */
static const uint16_t qr_shades[]
    = { UINT16_C(0x1082), UINT16_C(0x4a49), UINT16_C(0x7bef), UINT16_C(0xc618), COLOR_WHITE };
#define QR_SHADE_COUNT (sizeof(qr_shades) / sizeof(qr_shades[0]))
static size_t qr_shade_index = QR_SHADE_COUNT - 1;
static uint16_t qr_light(void) { return qr_shades[qr_shade_index]; }

void seedtool_render_qr_cycle_shade(void)
{
    qr_shade_index = qr_shade_index ? qr_shade_index - 1 : QR_SHADE_COUNT - 1;
}

/* In the gap seedtool_render_screen leaves between the transcript line (ends
 * y=81) and the footer (starts y=111). */
#define DICE_BAR_MARGIN 20
#define DICE_BAR_X DICE_BAR_MARGIN
#define DICE_BAR_WIDTH (SEEDTOOL_DISPLAY_WIDTH - 2 * DICE_BAR_MARGIN)
#define DICE_BAR_Y 90
#define DICE_BAR_HEIGHT 14

/* Left margin for the four-row body: a numbered list reads as a table, so every
 * row starts at the same x. */
#define SCREEN4_TEXT_X 20

#define KEYBOARD_COLUMNS 10
#define KEY_WIDTH (SEEDTOOL_DISPLAY_WIDTH / KEYBOARD_COLUMNS)
#define KEY_HEIGHT 27
#define KEYBOARD_TOP (SEEDTOOL_DISPLAY_HEIGHT - 3 * KEY_HEIGHT)

/* In the 6px gap between the typed text (ends y=48) and the keyboard (starts
 * y=54), with the entropy bar's margins. */
#define WORD_BAR_Y (KEYBOARD_TOP - 4)
#define WORD_BAR_HEIGHT 3

/* The 16px title, three 33px rows and the 11px footer fill the 135px display.
 * The scroll strip on the right is kept clear of the selection bar. */
#define LIST_TITLE_Y 3
#define LIST_TOP 21

/* The band between a plain screen's title and footer; the digit field centres
 * in it. */
#define SCREEN_TITLE_Y 2
#define SCREEN_TITLE_BOTTOM (SCREEN_TITLE_Y + 16) /* the 16px face's own height */
#define SCREEN_FOOTER_Y 111
#define LIST_GUTTER 10
#define LIST_BAR_X 2
#define LIST_BAR_WIDTH (SEEDTOOL_DISPLAY_WIDTH - LIST_GUTTER - 2 * LIST_BAR_X)
#define LIST_TEXT_X 6
#define LIST_TEXT_WIDTH (LIST_BAR_X + LIST_BAR_WIDTH - LIST_TEXT_X)
#define LIST_SCROLL_X (SEEDTOOL_DISPLAY_WIDTH - 7)
#define LIST_SCROLL_WIDTH 4
#define LIST_SCROLL_MIN 10

/* Nav chrome. The back arrow sits inside the title bar, and the title is
 * centred between margins of NAV_BACK_WIDTH so the arrow does not push it off
 * centre. */
#define NAV_BACK_X 2
#define NAV_BACK_Y 1
#define NAV_BACK_WIDTH 20
#define NAV_BACK_HEIGHT 19
#define NAV_ARROW_WIDTH 7
#define NAV_ARROW_HEIGHT 11
/* The tick's box is derived from its legs, so the two cannot drift. */
#define NAV_TICK_STROKE 2
#define NAV_TICK_SHORT 3
#define NAV_TICK_LONG 6
#define NAV_TICK_WIDTH (NAV_TICK_SHORT + NAV_TICK_LONG + NAV_TICK_STROKE)
#define NAV_TICK_HEIGHT (NAV_TICK_LONG + NAV_TICK_STROKE)
#define NAV_ROW_HEIGHT 32
/* The rows' height less the 2px gap under the last row, so the track stops
 * short of what follows. */
#define NAV_SCROLL_HEIGHT (SEEDTOOL_LIST_ROWS * NAV_ROW_HEIGHT - 2)
/* Between the lowest line a paged screen draws (ends y=103) and the bottom of
 * the display; the small face fits, the 16px one would not. */
#define NAV_COUNTER_Y 105
/* Four left-aligned rows, 28 to 88 in steps of 20: four 16px lines between the
 * header and the counter with a gap under each. */
#define NAV_ROWS_SHOWN 4
#define NAV_ROWS_TOP 28
#define NAV_ROWS_HEIGHT 20

/* Version 6 holds 134 bytes at ECC_LOW, enough for a key origin and an account
 * xpub in one code. Raising it is a compile error rather than a code clipped by
 * the display. */
#define QR_VERSION 6
#define QR_MAX_MODULES (17 + 4 * QR_VERSION)
#define QR_MARGIN 3
#define QR_TITLE_Y 50
_Static_assert(SEEDTOOL_DISPLAY_HEIGHT / (QR_MAX_MODULES + 2) >= 1, "the largest QR no longer fits the display");

/* Stackbit 1248 punch grid: four digit columns (thousands to units) by four
 * weight rows (1, 2, 4, 8), with the number and the word beside it. */
#define STACKBIT_GRID_X 10
#define STACKBIT_LABEL_WIDTH 12
#define STACKBIT_CELL 20
#define STACKBIT_DOT 6
#define STACKBIT_DIGITS 4
#define STACKBIT_ROWS 4
#define STACKBIT_GRID_TOP 26
#define STACKBIT_GRID_WIDTH (STACKBIT_LABEL_WIDTH + STACKBIT_DIGITS * STACKBIT_CELL)
#define STACKBIT_GRID_HEIGHT (STACKBIT_ROWS * STACKBIT_CELL)
#define STACKBIT_PANEL_X (STACKBIT_GRID_X + STACKBIT_GRID_WIDTH + 14)
#define STACKBIT_PANEL_WIDTH (SEEDTOOL_DISPLAY_WIDTH - STACKBIT_PANEL_X - 6)
#define STACKBIT_NUMBER_Y 40
#define STACKBIT_WORD_Y 68
#define STACKBIT_FOOTER_Y 122
_Static_assert(STACKBIT_GRID_TOP + STACKBIT_GRID_HEIGHT <= STACKBIT_FOOTER_Y, "the punch grid runs into the footer");
_Static_assert(STACKBIT_PANEL_WIDTH >= 60, "no room left beside the grid to name the word");

/* The physical plate's layout (Krux, src/krux/pages/stack_1248.py): two rows.
 * The thousands digit is one column of two cells (weights 1 and 2, since it is
 * never above 2); the others are 2x2 blocks weighted 1, 2 / 4, 8. Below the
 * grid, being wide and short. */
#define STACKBIT_PHYS_CELL 22
#define STACKBIT_PHYS_DOT 6
#define STACKBIT_PHYS_GAP 6
#define STACKBIT_PHYS_BLOCK_WIDTH (2 * STACKBIT_PHYS_CELL)
#define STACKBIT_PHYS_GRID_WIDTH \
    (STACKBIT_PHYS_CELL + STACKBIT_PHYS_GAP + 3 * STACKBIT_PHYS_BLOCK_WIDTH + 2 * STACKBIT_PHYS_GAP)
#define STACKBIT_PHYS_GRID_HEIGHT (2 * STACKBIT_PHYS_CELL)
#define STACKBIT_PHYS_GRID_X ((SEEDTOOL_DISPLAY_WIDTH - STACKBIT_PHYS_GRID_WIDTH) / 2)
#define STACKBIT_PHYS_GRID_TOP 28
#define STACKBIT_PHYS_NUMBER_Y 80
#define STACKBIT_PHYS_WORD_Y 102
#define STACKBIT_PHYS_FOOTER_Y 122
_Static_assert(STACKBIT_PHYS_GRID_X >= 0, "the physical punch grid is wider than the display");
_Static_assert(STACKBIT_PHYS_GRID_TOP + STACKBIT_PHYS_GRID_HEIGHT <= STACKBIT_PHYS_NUMBER_Y,
    "the physical punch grid runs into the number line");

extern const unsigned char tft_DefaultFont[];
extern const unsigned char tft_Ubuntu16[];

typedef struct {
    uint8_t y_offset;
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    uint8_t x_advance;
    const uint8_t* bitmap;
} glyph_t;

_Alignas(4) static uint16_t framebuffer[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];

static void fill_rect(int x, int y, int width, int height, const uint16_t color)
{
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > SEEDTOOL_DISPLAY_WIDTH) {
        width = SEEDTOOL_DISPLAY_WIDTH - x;
    }
    if (y + height > SEEDTOOL_DISPLAY_HEIGHT) {
        height = SEEDTOOL_DISPLAY_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint16_t* out = framebuffer + (y + row) * SEEDTOOL_DISPLAY_WIDTH + x;
        for (int col = 0; col < width; ++col) {
            out[col] = color;
        }
    }
}

static bool find_glyph(const uint8_t* font, const unsigned char character, glyph_t* glyph)
{
    const uint8_t* cursor = font + 4;
    while (*cursor != character && *cursor != 0xff) {
        const size_t bitmap_size = cursor[2] ? ((size_t)cursor[2] * cursor[3] + 7) / 8 : 0;
        cursor += 6 + bitmap_size;
    }
    if (*cursor == 0xff) {
        return false;
    }
    glyph->y_offset = cursor[1];
    glyph->width = cursor[2];
    glyph->height = cursor[3];
    glyph->x_offset = cursor[4] < 0x80 ? (int8_t)cursor[4] : (int8_t)(cursor[4] - 0x100);
    glyph->x_advance = cursor[5];
    glyph->bitmap = cursor + 6;
    return true;
}

static int glyph_advance(const uint8_t* font, const unsigned char character)
{
    glyph_t glyph;
    return find_glyph(font, character, &glyph) ? (glyph.width > glyph.x_advance ? glyph.width : glyph.x_advance) + 1
                                               : 0;
}

static int text_width(const uint8_t* font, const char* text, const size_t length)
{
    int width = 0;
    for (size_t i = 0; i < length; ++i) {
        width += glyph_advance(font, (unsigned char)text[i]);
    }
    return width ? width - 1 : 0;
}

static void draw_glyph(
    const uint8_t* font, const unsigned char character, const int x, const int y, const uint16_t color)
{
    glyph_t glyph;
    if (!find_glyph(font, character, &glyph)) {
        return;
    }
    size_t bit = 0;
    for (uint8_t row = 0; row < glyph.height; ++row) {
        for (uint8_t col = 0; col < glyph.width; ++col, ++bit) {
            if (glyph.bitmap[bit / 8] & (UINT8_C(0x80) >> (bit % 8))) {
                fill_rect(x + glyph.x_offset + col, y + glyph.y_offset + row, 1, 1, color);
            }
        }
    }
}

static void draw_line_at(
    const uint8_t* font, const char* text, const size_t length, int x, const int y, const uint16_t color)
{
    for (size_t i = 0; i < length; ++i) {
        draw_glyph(font, (unsigned char)text[i], x, y, color);
        x += glyph_advance(font, (unsigned char)text[i]);
    }
}

/* Centre `text` inside the box of `width` pixels starting at `x`. */
static void draw_centered_in(const uint8_t* font, const char* text, const int x, const int width, const int y,
    const uint16_t color)
{
    const size_t length = strlen(text);
    int offset = (width - text_width(font, text, length)) / 2;
    if (offset < 0) {
        offset = 0;
    }
    draw_line_at(font, text, length, x + offset, y, color);
}

static void draw_centered_line(
    const uint8_t* font, const char* text, const size_t length, const int x, const int width, const int y)
{
    int offset = (width - text_width(font, text, length)) / 2;
    if (offset < 0) {
        offset = 0;
    }
    draw_line_at(font, text, length, x + offset, y, COLOR_WHITE);
}

/* Centred and wrapped inside the box of `width` pixels at `x`, breaking at the
 * last space rather than inside a word; a word wider than the box is split.
 * Returns the y just past the last line, for callers stacking text below. */
static int draw_centered_box(const uint8_t* font, const char* text, const int x, const int width, int y)
{
    if (!text) {
        return y;
    }
    const int line_height = font[1];
    const char* cursor = text;
    while (*cursor && y + line_height <= SEEDTOOL_DISPLAY_HEIGHT) {
        const char* end = cursor;
        const char* last_space = NULL;
        const char* last_slash = NULL;
        int used = 0;
        while (*end && *end != '\n') {
            const int advance = glyph_advance(font, (unsigned char)*end);
            if (end != cursor && used + advance > width) {
                break;
            }
            if (*end == ' ') {
                last_space = end;
            } else if (*end == '/') {
                last_slash = end;
            }
            used += advance;
            ++end;
        }
        const bool mid_word = end > cursor && end[-1] != ' ' && *end && *end != '\n' && *end != ' ';
        /* A word breaks at the space before it, or a derivation path after
         * its last slash that fits, whichever is later: a path has no spaces,
         * and cut anywhere else it splits an index in two. */
        const char* line_end = end;
        if (mid_word) {
            const char* const at_space = last_space != cursor ? last_space : NULL;
            const char* const after_slash = last_slash && last_slash + 1 < end ? last_slash + 1 : NULL;
            if (after_slash && (!at_space || after_slash > at_space)) {
                line_end = after_slash;
            } else if (at_space) {
                line_end = at_space;
            }
        }
        draw_centered_line(font, cursor, (size_t)(line_end - cursor), x, width, y);
        cursor = *line_end == ' ' || *line_end == '\n' ? line_end + 1 : line_end;
        y += line_height;
    }
    return y;
}

static void draw_centered(const uint8_t* font, const char* text, const int y)
{
    draw_centered_box(font, text, 0, SEEDTOOL_DISPLAY_WIDTH, y);
}

/* Left-aligned, so a numbered list reads as a table. */
static void draw_left(const uint8_t* font, const char* text, const int x, const int y)
{
    draw_line_at(font, text, strlen(text), x, y, COLOR_WHITE);
}

/* What has been typed, in the larger face to be checked. Only the tail that
 * fits one line is drawn, so a long passphrase does not push into the keyboard. */
static void draw_typed(const uint8_t* font, const char* text, const int y)
{
    const size_t length = strlen(text);
    size_t start = 0;
    while (start < length && text_width(font, text + start, length - start) > SEEDTOOL_DISPLAY_WIDTH - 4) {
        ++start;
    }
    draw_centered_line(font, text + start, length - start, 0, SEEDTOOL_DISPLAY_WIDTH, y);
}

/* Leading characters of `text` that fit in `max_width` pixels, at most `limit`.
 * The font is proportional, so text is measured, not counted. */
static size_t fit_in(const uint8_t* font, const char* text, const size_t limit, const int max_width)
{
    int width = 0;
    size_t count = 0;
    for (; count < limit && text[count]; ++count) {
        const int advance = glyph_advance(font, (unsigned char)text[count]);
        if (width + advance > max_width) {
            break;
        }
        width += advance;
    }
    return count;
}

/* fit_in, backed off to the last space, so paged text never splits a word. Only
 * for callers with a next line to carry it onto; a list row keeps the plain
 * cut. */
static size_t fit_in_wrapped(const uint8_t* font, const char* text, const size_t limit, const int max_width)
{
    const size_t fit = fit_in(font, text, limit, max_width);
    if (!fit || !text[fit] || text[fit] == ' ') {
        return fit;
    }
    for (size_t i = fit; i > 0; --i) {
        if (text[i - 1] == ' ') {
            return i;
        }
    }
    return fit;
}

size_t seedtool_render_fit(const char* text, const size_t limit)
{
    return fit_in_wrapped(tft_Ubuntu16, text, limit, SEEDTOOL_DISPLAY_WIDTH - 4);
}

/* An address has no word boundaries to keep one's place by, so it is drawn in
 * groups of four with a gap and an alternating ink: two signals for where a
 * group ends. Grouping is only drawn; what is compared, hashed and encoded is
 * the unbroken string. */
#define GROUP_LEN SEEDTOOL_GROUP_LEN
#define GROUP_GAP 5

/* Ink for the group at `index`, counted from the start of the whole value so
 * the alternation carries across a line break rather than restarting. */
static uint16_t group_ink(const size_t index) { return index % 2 ? COLOR_HIGHLIGHT : COLOR_WHITE; }

/* `length` characters of `text` drawn in groups, centred in the column at
 * `column_x`. `first_group` is how many groups came before this line. */
static void draw_grouped_in(const uint8_t* font, const char* text, const size_t length, const int column_x,
    const int column_width, const int y, const size_t first_group)
{
    int width = 0;
    for (size_t i = 0; i < length; ++i) {
        width += glyph_advance(font, (unsigned char)text[i]);
    }
    /* Every gap but the trailing one: a line ending on a group boundary would
     * otherwise be centred as though it carried a group it does not. */
    const size_t groups = (length + GROUP_LEN - 1) / GROUP_LEN;
    if (groups > 1) {
        width += (int)(groups - 1) * GROUP_GAP;
    }
    int x = column_x + (column_width - width) / 2;
    if (x < column_x) {
        x = column_x;
    }
    for (size_t i = 0; i < length; ++i) {
        if (i && i % GROUP_LEN == 0) {
            x += GROUP_GAP;
        }
        const uint16_t ink = group_ink(first_group + i / GROUP_LEN);
        draw_glyph(font, (unsigned char)text[i], x, y, ink);
        x += glyph_advance(font, (unsigned char)text[i]);
    }
}

/* Characters of `text` that fit one line with the gaps paid for, rounded down
 * to whole groups unless the value ends mid-group. */
/* The width-and-face-agnostic half of seedtool_render_fit_grouped, so the
 * address beside a QR can be fitted to its own margin with the same rounding. */
static size_t fit_grouped_in(const uint8_t* font, const char* text, const size_t limit, const int max_width)
{
    int width = 0;
    size_t count = 0;
    for (; count < limit && text[count]; ++count) {
        int advance = glyph_advance(font, (unsigned char)text[count]);
        if (count && count % GROUP_LEN == 0) {
            advance += GROUP_GAP;
        }
        if (width + advance > max_width) {
            break;
        }
        width += advance;
    }
    if (text[count] && count >= GROUP_LEN) {
        count -= count % GROUP_LEN;
    }
    return count;
}

size_t seedtool_render_fit_grouped(const char* text, const size_t limit)
{
    return fit_grouped_in(tft_Ubuntu16, text, limit, SEEDTOOL_DISPLAY_WIDTH - 4);
}


/* A row's text, with an ellipsis when it did not all fit, so a cut address does
 * not look like a whole one. The ellipsis comes out of the same width. */
#define ROW_ELLIPSIS "..."

static void draw_row(const uint8_t* font, const char* text, const int x, const int y, const int width,
    const uint16_t ink)
{
    const size_t fit = fit_in(font, text, SIZE_MAX, width);
    if (!text[fit]) {
        draw_line_at(font, text, fit, x, y, ink);
        return;
    }
    const int dots = text_width(font, ROW_ELLIPSIS, strlen(ROW_ELLIPSIS));
    const size_t shortened = fit_in(font, text, SIZE_MAX, width - dots);
    draw_line_at(font, text, shortened, x, y, ink);
    draw_line_at(font, ROW_ELLIPSIS, strlen(ROW_ELLIPSIS), x + text_width(font, text, shortened), y, ink);
}

size_t seedtool_render_fit_row(const char* text) { return fit_in(tft_Ubuntu16, text, SIZE_MAX, LIST_TEXT_WIDTH); }

size_t seedtool_render_fit_tail(const char* text)
{
    const size_t length = strlen(text);
    size_t start = 0;
    while (start < length && text_width(tft_Ubuntu16, text + start, length - start) > SEEDTOOL_DISPLAY_WIDTH - 4) {
        ++start;
    }
    return length - start;
}

void seedtool_render_clear(void) { fill_rect(0, 0, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT, COLOR_BLACK); }

/* The splash is the logo, and nothing else: it holds the screen briefly and the
 * menu follows. */
_Static_assert(SEEDTOOL_LOGO_WIDTH <= SEEDTOOL_DISPLAY_WIDTH, "the logo is wider than the display");
_Static_assert(SEEDTOOL_LOGO_HEIGHT <= SEEDTOOL_DISPLAY_HEIGHT, "the logo is taller than the display");

/* Sixteen palette entries, two pixels per byte, high nibble first. */
static void draw_logo(const int x, const int y)
{
    for (int row = 0; row < SEEDTOOL_LOGO_HEIGHT; ++row) {
        uint16_t* const out = framebuffer + (y + row) * SEEDTOOL_DISPLAY_WIDTH + x;
        for (int column = 0; column < SEEDTOOL_LOGO_WIDTH; ++column) {
            const int index = row * SEEDTOOL_LOGO_WIDTH + column;
            const uint8_t packed = seedtool_logo_pixels[index / 2];
            out[column] = seedtool_logo_palette[index % 2 ? packed & 0x0f : packed >> 4];
        }
    }
}

void seedtool_render_splash(void)
{
    seedtool_render_clear();
    draw_logo((SEEDTOOL_DISPLAY_WIDTH - SEEDTOOL_LOGO_WIDTH) / 2,
        (SEEDTOOL_DISPLAY_HEIGHT - SEEDTOOL_LOGO_HEIGHT) / 2);
}

void seedtool_render_screen(const char* title, const char* line1, const char* line2, const char* footer)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, 5);
    /* The body carries the values that get transcribed, so it takes the larger
     * face. The footer stays small or it would run off the display. */
    draw_centered(tft_Ubuntu16, line1, 39);
    draw_centered(tft_Ubuntu16, line2, 65);
    draw_centered(tft_DefaultFont, footer, SCREEN_FOOTER_Y);
}

seedtool_thumb_t seedtool_list_thumb(const size_t count, const size_t top, const int track)
{
    seedtool_thumb_t thumb = { 0, track };
    if (count <= SEEDTOOL_LIST_ROWS) {
        return thumb;
    }
    thumb.height = (int)((size_t)track * SEEDTOOL_LIST_ROWS / count);
    if (thumb.height < LIST_SCROLL_MIN) {
        thumb.height = LIST_SCROLL_MIN;
    }
    /* The thumb sits against the top at the first row and against the bottom at
     * the last; a small top that the division floors to 0 is nudged off the
     * top, so row 1 never reads as row 0. */
    thumb.offset = (int)((size_t)(track - thumb.height) * top / (count - SEEDTOOL_LIST_ROWS));
    if (top && !thumb.offset) {
        thumb.offset = 1;
    }
    return thumb;
}

size_t seedtool_list_top(const size_t count, const size_t selected, size_t previous_top)
{
    if (count <= SEEDTOOL_LIST_ROWS) {
        return 0;
    }
    /* Never leave blank rows below the last item: the end of a list has to look
     * like the end of it. */
    const size_t last_top = count - SEEDTOOL_LIST_ROWS;
    if (previous_top > last_top) {
        previous_top = last_top;
    }
    if (selected < previous_top) {
        return selected;
    }
    if (selected >= previous_top + SEEDTOOL_LIST_ROWS) {
        return selected - SEEDTOOL_LIST_ROWS + 1;
    }
    return previous_top;
}

/* A solid triangle filling a width x height box, apex the way asked, drawn
 * because the faces have no glyph for it. Up and down draw rows, left and right
 * the same arithmetic in columns. The `+ 1` on the step keeps the apex a single
 * pixel. */
typedef enum { TRIANGLE_UP, TRIANGLE_DOWN, TRIANGLE_LEFT, TRIANGLE_RIGHT } triangle_dir_t;

static void draw_triangle(
    const int x, const int y, const int width, const int height, const triangle_dir_t dir, const uint16_t color)
{
    const bool vertical = dir == TRIANGLE_UP || dir == TRIANGLE_DOWN;
    const int steps = vertical ? height : width;
    const int across = vertical ? width : height;
    for (int i = 0; i < steps; ++i) {
        /* DOWN and RIGHT are UP and LEFT walked from the other end. */
        const int from_point = dir == TRIANGLE_DOWN || dir == TRIANGLE_RIGHT ? steps - 1 - i : i;
        const int half = (from_point + 1) * across / (2 * steps);
        if (vertical) {
            fill_rect(x + width / 2 - half, y + i, 2 * half + 1, 1, color);
        } else {
            fill_rect(x + i, y + height / 2 - half, 1, 2 * half + 1, color);
        }
    }
}

/* A tick, drawn because the faces have no glyph for it: two strokes from a
 * common low point, each a stack of squares so the diagonals read as solid. `x,
 * y` is the top-left of the NAV_TICK_WIDTH x NAV_TICK_HEIGHT box. */
static void draw_tick(const int x, const int y, const uint16_t color)
{
    const int foot_x = x + NAV_TICK_SHORT;
    const int foot_y = y + NAV_TICK_LONG;
    for (int i = 0; i <= NAV_TICK_SHORT; ++i) {
        fill_rect(foot_x - i, foot_y - i, NAV_TICK_STROKE, NAV_TICK_STROKE, color);
    }
    for (int i = 0; i <= NAV_TICK_LONG; ++i) {
        fill_rect(foot_x + i, foot_y - i, NAV_TICK_STROKE, NAV_TICK_STROKE, color);
    }
}

static void draw_border(const int x, const int y, const int width, const int height, const uint16_t color)
{
    fill_rect(x, y, width, 1, color);
    fill_rect(x, y + height - 1, width, 1, color);
    fill_rect(x, y, 1, height, color);
    fill_rect(x + width - 1, y, 1, height, color);
}

/* The chrome, drawn once for every screen that wears it. */
static void draw_nav_header(const seedtool_nav_t* nav, const char* title)
{
    /* A screen with no way back gets no arrow, and its title takes the whole
     * width rather than a column with a gap held open beside it. */
    const int inset = nav->back ? NAV_BACK_X + NAV_BACK_WIDTH : 0;
    if (nav->back) {
        const bool on_back = nav->selected == SEEDTOOL_NAV_BACK;
        if (on_back) {
            fill_rect(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, COLOR_HIGHLIGHT);
        }
        /* Sides and top, no bottom: the header's closing rule is the fourth
         * edge, as in Jade (ui/dialogs.c). Bordered even when idle, so it reads
         * as a control. */
        const uint16_t edge = on_back ? COLOR_HIGHLIGHT : COLOR_DIM;
        fill_rect(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, 1, edge);
        fill_rect(NAV_BACK_X, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        fill_rect(NAV_BACK_X + NAV_BACK_WIDTH - 1, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        draw_triangle(NAV_BACK_X + (NAV_BACK_WIDTH - NAV_ARROW_WIDTH) / 2,
            NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_ARROW_HEIGHT) / 2, NAV_ARROW_WIDTH, NAV_ARROW_HEIGHT, TRIANGLE_LEFT,
            on_back ? COLOR_BLACK : COLOR_WHITE);
    }
    /* The confirm in the right slot, mirroring the arrow: same box, same
     * borders, same rule closing it underneath. */
    if (nav->confirm != SEEDTOOL_CONFIRM_NONE) {
        /* Filled when available and selected, outlined when available, dim
         * when confirming is not possible yet. */
        const bool on_tick = nav->confirm_enabled && nav->selected == SEEDTOOL_NAV_CONFIRM;
        const int x = SEEDTOOL_DISPLAY_WIDTH - NAV_BACK_X - NAV_BACK_WIDTH;
        if (on_tick) {
            fill_rect(x, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, COLOR_HIGHLIGHT);
        }
        const uint16_t edge = on_tick ? COLOR_HIGHLIGHT : COLOR_DIM;
        fill_rect(x, NAV_BACK_Y, NAV_BACK_WIDTH, 1, edge);
        fill_rect(x, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        fill_rect(x + NAV_BACK_WIDTH - 1, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        const uint16_t ink = !nav->confirm_enabled ? COLOR_DIM : on_tick ? COLOR_BLACK : COLOR_WHITE;
        if (nav->confirm == SEEDTOOL_CONFIRM_FORWARD) {
            /* The back arrow's shape, mirrored: the same control pointing the
             * other way. */
            draw_triangle(x + (NAV_BACK_WIDTH - NAV_ARROW_WIDTH) / 2,
                NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_ARROW_HEIGHT) / 2, NAV_ARROW_WIDTH, NAV_ARROW_HEIGHT,
                TRIANGLE_RIGHT, ink);
        } else {
            draw_tick(x + (NAV_BACK_WIDTH - NAV_TICK_WIDTH) / 2,
                NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_TICK_HEIGHT) / 2, ink);
        }
    }
    /* Centred between margins the width of the arrow's box, so a long title
     * does not paint into it. */
    (void)draw_centered_box(tft_Ubuntu16, title, inset, SEEDTOOL_DISPLAY_WIDTH - 2 * inset, LIST_TITLE_Y);
    /* The rule under the header, which closes the arrow's box and the
     * confirm's, drawn here so every screen wearing the header gets it. */
    fill_rect(NAV_BACK_X, NAV_BACK_Y + NAV_BACK_HEIGHT, SEEDTOOL_DISPLAY_WIDTH - 2 * NAV_BACK_X, 1, COLOR_DIM);
}

/* The page counter, in the gap the body leaves above the confirm bar. Small
 * face: it is a position indicator, not a value to transcribe. */
static void draw_nav_counter(const seedtool_nav_t* nav)
{
    if (nav->counter) {
        draw_centered_in(tft_DefaultFont, nav->counter, 0, SEEDTOOL_DISPLAY_WIDTH, NAV_COUNTER_Y, COLOR_WHITE);
    }
}

/* The order every nav screen draws in, written once: clear, chrome, body,
 * counter. */
static void nav_begin(const seedtool_nav_t* nav, const char* title)
{
    seedtool_render_clear();
    draw_nav_header(nav, title);
}

static void nav_end(const seedtool_nav_t* nav)
{
    draw_nav_counter(nav);
}

void seedtool_render_nav_text(
    const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2, const char* line3)
{
    nav_begin(nav, title);
    /* Two lines at 39 and 65, or three at 33, 58 and 83, pitched tighter to
     * clear the counter. */
    if (line3) {
        draw_centered(tft_Ubuntu16, line1, 33);
        draw_centered(tft_Ubuntu16, line2, 58);
        draw_centered(tft_Ubuntu16, line3, 83);
    } else {
        draw_centered(tft_Ubuntu16, line1, 39);
        draw_centered(tft_Ubuntu16, line2, 65);
    }
    nav_end(nav);
}

void seedtool_render_nav_grouped(const seedtool_nav_t* nav, const char* title, const char* const* lines,
    const size_t* first_group, const size_t count)
{
    nav_begin(nav, title);
    /* Always the three-line pitch, so a page does not move its text when a
     * third line appears. */
    static const int y[] = { 33, 58, 83 };
    for (size_t i = 0; i < count && i < 3; ++i) {
        if (lines[i] && lines[i][0]) {
            draw_grouped_in(
                tft_Ubuntu16, lines[i], strlen(lines[i]), 0, SEEDTOOL_DISPLAY_WIDTH, y[i], first_group[i]);
        }
    }
    nav_end(nav);
}

void seedtool_render_nav_rows(
    const seedtool_nav_t* nav, const char* title, const char* const* rows, const size_t count)
{
    nav_begin(nav, title);
    for (size_t row = 0; row < NAV_ROWS_SHOWN && row < count; ++row) {
        draw_left(tft_Ubuntu16, rows[row], SCREEN4_TEXT_X, NAV_ROWS_TOP + (int)row * NAV_ROWS_HEIGHT);
    }
    nav_end(nav);
}

void seedtool_render_nav_list(
    const seedtool_nav_t* nav, const char* title, const char* const* items, const size_t count, const size_t top)
{
    nav_begin(nav, title);
    for (size_t row = 0; row < SEEDTOOL_LIST_ROWS && top + row < count; ++row) {
        const char* const item = items[top + row];
        const bool highlighted = top + row == nav->selected;
        const int y = LIST_TOP + (int)row * NAV_ROW_HEIGHT;
        /* A rule above every row, so they read as cells (as in Jade's menus,
         * ui/dialogs.c). It sits in the gap between cells, clear of the
         * selection bar. */
        fill_rect(LIST_BAR_X, y - 1, LIST_BAR_WIDTH, 1, COLOR_DIM);
        if (highlighted) {
            fill_rect(LIST_BAR_X, y, LIST_BAR_WIDTH, NAV_ROW_HEIGHT - 2, COLOR_HIGHLIGHT);
        }
        draw_row(tft_Ubuntu16, item, LIST_TEXT_X, y + (NAV_ROW_HEIGHT - tft_Ubuntu16[1]) / 2, LIST_TEXT_WIDTH,
            highlighted ? COLOR_BLACK : COLOR_WHITE);
    }
    if (count > SEEDTOOL_LIST_ROWS) {
        const seedtool_thumb_t thumb = seedtool_list_thumb(count, top, NAV_SCROLL_HEIGHT);
        fill_rect(LIST_SCROLL_X, LIST_TOP, LIST_SCROLL_WIDTH, NAV_SCROLL_HEIGHT, COLOR_DIM);
        fill_rect(LIST_SCROLL_X, LIST_TOP + thumb.offset, LIST_SCROLL_WIDTH, thumb.height, COLOR_WHITE);
    }
    nav_end(nav);
}

/* One past the last key of the row starting at `row`. */
static const char* row_end(const char* row)
{
    while (*row && *row != '\n') {
        ++row;
    }
    return row;
}

/* Every key is found by its character rather than by where it sits, so the
 * layouts are QWERTY, or anything else, purely by changing these strings. */
const char* const seedtool_passphrase_layouts[SEEDTOOL_PASSPHRASE_PAGES] = {
    "qwertyuiop\nasdfghjkl \nzxcvbnm\b\t\r",
    "QWERTYUIOP\nASDFGHJKL \nZXCVBNM\b\t\r",
    "1234567890\n!\"#$%&'()\n*+,-./ \b\t\r",
    ":;<=>?@\n[\\]^_`~\n{|} \b\t\r",
};

/* The letter pages in alphabetical rows; punctuation has no such order, so
 * the digit and symbol pages are the same. */
const char* const seedtool_passphrase_layouts_abc[SEEDTOOL_PASSPHRASE_PAGES] = {
    "abcdefghij\nklmnopqrst\nuvwxyz \b\t\r",
    "ABCDEFGHIJ\nKLMNOPQRST\nUVWXYZ \b\t\r",
    "1234567890\n!\"#$%&'()\n*+,-./ \b\t\r",
    ":;<=>?@\n[\\]^_`~\n{|} \b\t\r",
};

size_t seedtool_layout_keys(const char* layout)
{
    size_t count = 0;
    for (const char* cursor = layout; *cursor; ++cursor) {
        if (*cursor != '\n') {
            ++count;
        }
    }
    return count;
}

char seedtool_layout_key(const char* layout, size_t index)
{
    for (const char* cursor = layout; *cursor; ++cursor) {
        if (*cursor != '\n' && !index--) {
            return *cursor;
        }
    }
    return '\0';
}

size_t seedtool_layout_center(const char* layout)
{
    size_t rows = 0;
    for (const char* cursor = layout; *cursor;) {
        const char* const end = row_end(cursor);
        ++rows;
        cursor = *end ? end + 1 : end;
    }
    if (!rows) {
        return 0;
    }
    const size_t middle = (rows - 1) / 2;
    size_t index = 0, row = 0;
    for (const char* cursor = layout; *cursor; ++row) {
        const char* const end = row_end(cursor);
        const size_t length = (size_t)(end - cursor);
        if (row == middle) {
            return length ? index + (length - 1) / 2 : index;
        }
        index += length;
        cursor = *end ? end + 1 : end;
    }
    return 0;
}

/* A backspace arrow, drawn because the fonts are printable ASCII only. */
#define BACKSPACE_WIDTH 15
#define BACKSPACE_HEIGHT 11

static void draw_backspace(const int x, const int y, const uint16_t ink, const uint16_t ground)
{
    const int half = BACKSPACE_HEIGHT / 2;
    for (int column = 0; column <= half; ++column) {
        fill_rect(x + column, y + half - column, 1, 2 * column + 1, ink);
    }
    fill_rect(x + half + 1, y, BACKSPACE_WIDTH - half - 1, BACKSPACE_HEIGHT, ink);
    /* An x cut out of the body, so the key reads as "delete" rather than as a
     * plain arrow that could just as well mean "go back". */
    for (int i = 0; i < 5; ++i) {
        fill_rect(x + 8 + i, y + 3 + i, 1, 1, ground);
        fill_rect(x + 8 + i, y + 7 - i, 1, 1, ground);
    }
}

static const char* key_label(const char key, char* scratch)
{
    switch (key) {
    case SEEDTOOL_KEY_PAGE:
        return ">>";
    case SEEDTOOL_KEY_ACCEPT:
        return "OK";
    case ' ':
        return "SP";
    default:
        scratch[0] = key;
        scratch[1] = '\0';
        return scratch;
    }
}

static int clamp_pct(int pct) { return pct < 0 ? 0 : pct > 100 ? 100 : pct; }

/* The entropy quality bar, for every screen with room above DICE_BAR_Y. NULL
 * draws nothing. */
static void draw_progress_bar(const seedtool_progress_t* progress)
{
    if (!progress) {
        return;
    }
    const int inner_x = DICE_BAR_X + 2;
    const int inner_width = DICE_BAR_WIDTH - 4;
    const int segment_height = (DICE_BAR_HEIGHT - 4) / 2;
    const int rolls_width = clamp_pct(progress->rolls_pct) * inner_width / 100;
    const int entropy_width = clamp_pct(progress->entropy_pct) * inner_width / 100;
    if (rolls_width > 0) {
        fill_rect(inner_x, DICE_BAR_Y + 2, rolls_width, segment_height, COLOR_HIGHLIGHT);
    }
    if (entropy_width > 0) {
        fill_rect(inner_x, DICE_BAR_Y + 2 + segment_height + 1, entropy_width, segment_height,
            progress->warn ? COLOR_WARN : COLOR_HIGHLIGHT);
    }
    draw_border(DICE_BAR_X, DICE_BAR_Y, DICE_BAR_WIDTH, DICE_BAR_HEIGHT, progress->complete ? COLOR_GO : COLOR_DIM);
}

/* A row of digit boxes, the one being set carrying up and down arrows, so the
 * value scrolls in place: up and down are the digit. Adapted from Jade's PIN
 * and index entry (main/ui/digit_entry.c). */
#define DIGIT_BOX_WIDTH 34
#define DIGIT_BOX_HEIGHT 32
#define DIGIT_BOX_GAP 6
#define DIGIT_ARROW_WIDTH 11
/* Six rows: an 11-wide triangle has exactly six distinct half-widths, and a
 * seventh would repeat one. */
#define DIGIT_ARROW_HEIGHT 6

/* Arrow, boxes, arrow: one stack, its height derived from its parts. */
#define DIGIT_FIELD_GAP 4
#define DIGIT_FIELD_HEIGHT (2 * (DIGIT_ARROW_HEIGHT + DIGIT_FIELD_GAP) + DIGIT_BOX_HEIGHT)
#define DIGIT_FIELD_TOP(bottom) (SCREEN_TITLE_BOTTOM + ((bottom) -SCREEN_TITLE_BOTTOM - DIGIT_FIELD_HEIGHT) / 2)
#define DIGIT_FIELD_BOX_Y(top) ((top) + DIGIT_ARROW_HEIGHT + DIGIT_FIELD_GAP)
#define DIGIT_FIELD_DOWN_Y(top) (DIGIT_FIELD_BOX_Y(top) + DIGIT_BOX_HEIGHT + DIGIT_FIELD_GAP)

/* The widest field, centred, must fit the display: a box or gap grown by a few
 * pixels would be clipped silently. The arrows must clear the title and the
 * footer. */
_Static_assert(SEEDTOOL_DIGIT_BOXES_MAX * DIGIT_BOX_WIDTH + (SEEDTOOL_DIGIT_BOXES_MAX - 1) * DIGIT_BOX_GAP
        <= SEEDTOOL_DISPLAY_WIDTH,
    "the digit boxes are wider than the display");
/* Both placements, each against the thing that actually bounds it. */
_Static_assert(DIGIT_FIELD_TOP(SCREEN_FOOTER_Y) >= SCREEN_TITLE_BOTTOM, "the digit field runs into the title");
_Static_assert(DIGIT_FIELD_DOWN_Y(DIGIT_FIELD_TOP(SCREEN_FOOTER_Y)) + DIGIT_ARROW_HEIGHT <= SCREEN_FOOTER_Y,
    "the digit field runs into the footer");
_Static_assert(DIGIT_FIELD_TOP(DICE_BAR_Y) >= SCREEN_TITLE_BOTTOM, "the value box runs into the title");
_Static_assert(DIGIT_FIELD_DOWN_Y(DIGIT_FIELD_TOP(DICE_BAR_Y)) + DIGIT_ARROW_HEIGHT <= DICE_BAR_Y,
    "the value box runs into the quality bar");


/* One wider box for a value scrolled whole, a die face or a D20. `back` puts
 * the way out on the ring, as the digit field does. */
#define VALUE_BOX_WIDTH 60

void seedtool_render_value_box(
    const char* title, const char* text, const bool back, const char* footer, const seedtool_progress_t* progress)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, SCREEN_TITLE_Y);
    /* Centred above the quality bar whether or not `progress` draws it, so the
     * box does not move between graded and ungraded runs. */
    const int top = DIGIT_FIELD_TOP(DICE_BAR_Y);
    const int box_y = DIGIT_FIELD_BOX_Y(top);
    const int x = (SEEDTOOL_DISPLAY_WIDTH - VALUE_BOX_WIDTH) / 2;
    fill_rect(x, box_y, VALUE_BOX_WIDTH, DIGIT_BOX_HEIGHT, COLOR_HIGHLIGHT);
    draw_border(x, box_y, VALUE_BOX_WIDTH, DIGIT_BOX_HEIGHT, COLOR_HIGHLIGHT);
    if (back) {
        draw_backspace(x + (VALUE_BOX_WIDTH - BACKSPACE_WIDTH) / 2,
            box_y + (DIGIT_BOX_HEIGHT - BACKSPACE_HEIGHT) / 2, COLOR_BLACK, COLOR_HIGHLIGHT);
    } else {
        draw_centered_in(tft_Ubuntu16, text, x, VALUE_BOX_WIDTH,
            box_y + (DIGIT_BOX_HEIGHT - tft_Ubuntu16[1]) / 2, COLOR_BLACK);
    }
    const int arrow_x = x + (VALUE_BOX_WIDTH - DIGIT_ARROW_WIDTH) / 2;
    draw_triangle(arrow_x, top, DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_UP, COLOR_WHITE);
    draw_triangle(arrow_x, DIGIT_FIELD_DOWN_Y(top), DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_DOWN, COLOR_WHITE);
    draw_centered(tft_DefaultFont, footer, SCREEN_FOOTER_Y);
    if (progress) {
        draw_progress_bar(progress);
    }
}

void seedtool_render_digits(const char* title, const char* digits, const size_t count, const size_t active,
    const char* footer, const seedtool_progress_t* progress)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, SCREEN_TITLE_Y);

    /* Nothing under this field but the footer, so it centres in that whole
     * band rather than sharing the value box's higher placement. */
    const int top = DIGIT_FIELD_TOP(SCREEN_FOOTER_Y);
    const int box_y = DIGIT_FIELD_BOX_Y(top);
    const int span = (int)count * DIGIT_BOX_WIDTH + ((int)count - 1) * DIGIT_BOX_GAP;
    const int left = (SEEDTOOL_DISPLAY_WIDTH - span) / 2;
    for (size_t i = 0; i < count; ++i) {
        const int x = left + (int)i * (DIGIT_BOX_WIDTH + DIGIT_BOX_GAP);
        const bool selected = i == active;
        const bool set = i < active;
        if (selected) {
            fill_rect(x, box_y, DIGIT_BOX_WIDTH, DIGIT_BOX_HEIGHT, COLOR_HIGHLIGHT);
        }
        draw_border(x, box_y, DIGIT_BOX_WIDTH, DIGIT_BOX_HEIGHT,
            selected ? COLOR_HIGHLIGHT : set ? COLOR_WHITE : COLOR_DIM);

        const char shown = digits[i];
        const int glyph_y = box_y + (DIGIT_BOX_HEIGHT - tft_Ubuntu16[1]) / 2;
        if (shown == SEEDTOOL_KEY_ACCEPT) {
            draw_centered_in(tft_Ubuntu16, "OK", x, DIGIT_BOX_WIDTH, glyph_y, selected ? COLOR_BLACK : COLOR_WHITE);
        } else if (shown == SEEDTOOL_KEY_BACKSPACE) {
            /* The way out rides the ring as the backspace glyph. */
            draw_backspace(x + (DIGIT_BOX_WIDTH - BACKSPACE_WIDTH) / 2, box_y + (DIGIT_BOX_HEIGHT - BACKSPACE_HEIGHT) / 2,
                COLOR_BLACK, COLOR_HIGHLIGHT);
        } else if (shown && shown != ' ') {
            const char text[2] = { shown, '\0' };
            draw_centered_in(tft_Ubuntu16, text, x, DIGIT_BOX_WIDTH, glyph_y, selected ? COLOR_BLACK : COLOR_WHITE);
        }

        if (selected) {
            const int arrow_x = x + (DIGIT_BOX_WIDTH - DIGIT_ARROW_WIDTH) / 2;
            draw_triangle(arrow_x, top, DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_UP, COLOR_WHITE);
            draw_triangle(
                arrow_x, DIGIT_FIELD_DOWN_Y(top), DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_DOWN, COLOR_WHITE);
        }
    }

    draw_centered(tft_DefaultFont, footer, SCREEN_FOOTER_Y);
    if (progress) {
        draw_progress_bar(progress);
    }
}

void seedtool_render_dice_screen(const char* title, const char* line1, const char* line2, const char* footer,
    const seedtool_progress_t* progress)
{
    seedtool_render_screen(title, line1, line2, footer);
    draw_progress_bar(progress);
}

void seedtool_render_nav_dice(const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2,
    const seedtool_progress_t* progress)
{
    /* The quality bar (y=90..103) takes the footer's row, which the chrome
     * replaces. */
    seedtool_render_nav_text(nav, title, line1, line2, NULL);
    draw_progress_bar(progress);
}


void seedtool_render_keyboard(const char* title, const char* text, const char* layout, const bool* enabled,
    const size_t selected, const size_t position, const size_t total)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, 2);
    draw_typed(tft_Ubuntu16, text, 32);
    if (total && position) {
        const size_t done = position > total ? total : position - 1;
        const int filled = (int)(DICE_BAR_WIDTH * done / total);
        fill_rect(DICE_BAR_X, WORD_BAR_Y, DICE_BAR_WIDTH, WORD_BAR_HEIGHT, COLOR_DIM);
        if (filled) {
            fill_rect(DICE_BAR_X, WORD_BAR_Y, filled, WORD_BAR_HEIGHT, COLOR_HIGHLIGHT);
        }
    }

    size_t key = 0;
    int y = KEYBOARD_TOP;
    for (const char* row = layout; *row;) {
        const char* const end = row_end(row);
        const int count = (int)(end - row);
        const int left = (SEEDTOOL_DISPLAY_WIDTH - count * KEY_WIDTH) / 2;
        for (int column = 0; column < count; ++column, ++key) {
            const bool active = !enabled || enabled[key];
            const bool highlighted = key == selected;
            const int x = left + column * KEY_WIDTH;
            char scratch[2];
            const char* const label = key_label(row[column], scratch);
            /* Letters and digits take the larger face; word keys keep the small
             * one, or "OK" would fill its cell. */
            const bool worded = row[column] == SEEDTOOL_KEY_PAGE || row[column] == SEEDTOOL_KEY_ACCEPT
                || row[column] == ' ';
            const unsigned char* const face = worded ? tft_DefaultFont : tft_Ubuntu16;
            if (highlighted) {
                fill_rect(x + 1, y + 1, KEY_WIDTH - 2, KEY_HEIGHT - 3, COLOR_HIGHLIGHT);
            } else {
                draw_border(x + 1, y + 1, KEY_WIDTH - 2, KEY_HEIGHT - 3, active ? COLOR_DIM : COLOR_BLACK);
            }
            const uint16_t ink = highlighted ? COLOR_BLACK : active ? COLOR_WHITE : COLOR_DIM;
            if (row[column] == SEEDTOOL_KEY_BACKSPACE) {
                draw_backspace(x + (KEY_WIDTH - BACKSPACE_WIDTH) / 2, y + (KEY_HEIGHT - BACKSPACE_HEIGHT) / 2 - 1,
                    ink, highlighted ? COLOR_HIGHLIGHT : COLOR_BLACK);
            } else {
                draw_centered_in(face, label, x, KEY_WIDTH, y + (KEY_HEIGHT - face[1]) / 2, ink);
            }
        }
        y += KEY_HEIGHT;
        row = *end ? end + 1 : end;
    }
}

void seedtool_render_stackbit_screen(
    const seedtool_nav_t* nav, const char* title, const unsigned word_number, const char* word, const char* footer)
{
    static const uint8_t WEIGHTS[STACKBIT_ROWS] = { 1, 2, 4, 8 };
    static const char WEIGHT_LABELS[] = "1248";

    nav_begin(nav, title);

    char digits[STACKBIT_DIGITS + 1];
    (void)snprintf(digits, sizeof(digits), "%04u", word_number);
    for (int col = 0; col < STACKBIT_DIGITS; ++col) {
        const unsigned value = (unsigned)(digits[col] - '0');
        for (int row = 0; row < STACKBIT_ROWS; ++row) {
            const int x = STACKBIT_GRID_X + STACKBIT_LABEL_WIDTH + col * STACKBIT_CELL;
            const int y = STACKBIT_GRID_TOP + row * STACKBIT_CELL;
            draw_border(x, y, STACKBIT_CELL, STACKBIT_CELL, COLOR_DIM);
            if (value & WEIGHTS[row]) {
                fill_rect(x + (STACKBIT_CELL - STACKBIT_DOT) / 2, y + (STACKBIT_CELL - STACKBIT_DOT) / 2,
                    STACKBIT_DOT, STACKBIT_DOT, COLOR_HIGHLIGHT);
            }
        }
    }
    for (int row = 0; row < STACKBIT_ROWS; ++row) {
        const int y = STACKBIT_GRID_TOP + row * STACKBIT_CELL + (STACKBIT_CELL - tft_DefaultFont[1]) / 2;
        draw_line_at(tft_DefaultFont, &WEIGHT_LABELS[row], 1, STACKBIT_GRID_X, y, COLOR_DIM);
    }
    draw_centered_in(tft_Ubuntu16, digits, STACKBIT_PANEL_X, STACKBIT_PANEL_WIDTH, STACKBIT_NUMBER_Y, COLOR_WHITE);
    draw_centered_in(tft_DefaultFont, word, STACKBIT_PANEL_X, STACKBIT_PANEL_WIDTH, STACKBIT_WORD_Y, COLOR_DIM);
    draw_centered(tft_DefaultFont, footer, STACKBIT_FOOTER_Y);
}

void seedtool_render_stackbit_physical_screen(
    const seedtool_nav_t* nav, const char* title, const unsigned word_number, const char* word, const char* footer)
{
    nav_begin(nav, title);

    char digits[STACKBIT_DIGITS + 1];
    (void)snprintf(digits, sizeof(digits), "%04u", word_number);

    int x = STACKBIT_PHYS_GRID_X;
    const unsigned thousands = (unsigned)(digits[0] - '0');
    for (int row = 0; row < 2; ++row) {
        const int y = STACKBIT_PHYS_GRID_TOP + row * STACKBIT_PHYS_CELL;
        const unsigned weight = row ? 2u : 1u;
        draw_border(x, y, STACKBIT_PHYS_CELL, STACKBIT_PHYS_CELL, COLOR_DIM);
        const char label[2] = { (char)('0' + weight), '\0' };
        draw_line_at(tft_DefaultFont, label, 1, x + 2, y + 1, COLOR_DIM);
        if (thousands & weight) {
            fill_rect(x + (STACKBIT_PHYS_CELL - STACKBIT_PHYS_DOT) / 2,
                y + (STACKBIT_PHYS_CELL - STACKBIT_PHYS_DOT) / 2, STACKBIT_PHYS_DOT, STACKBIT_PHYS_DOT,
                COLOR_HIGHLIGHT);
        }
    }
    x += STACKBIT_PHYS_CELL + STACKBIT_PHYS_GAP;

    static const unsigned BLOCK_WEIGHTS[2][2] = { { 1, 2 }, { 4, 8 } };
    for (int digit = 1; digit < STACKBIT_DIGITS; ++digit) {
        const unsigned value = (unsigned)(digits[digit] - '0');
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const int cx = x + col * STACKBIT_PHYS_CELL;
                const int cy = STACKBIT_PHYS_GRID_TOP + row * STACKBIT_PHYS_CELL;
                const unsigned weight = BLOCK_WEIGHTS[row][col];
                draw_border(cx, cy, STACKBIT_PHYS_CELL, STACKBIT_PHYS_CELL, COLOR_DIM);
                const char label[2] = { (char)('0' + weight), '\0' };
                draw_line_at(tft_DefaultFont, label, 1, cx + 2, cy + 1, COLOR_DIM);
                if (value & weight) {
                    fill_rect(cx + (STACKBIT_PHYS_CELL - STACKBIT_PHYS_DOT) / 2,
                        cy + (STACKBIT_PHYS_CELL - STACKBIT_PHYS_DOT) / 2, STACKBIT_PHYS_DOT, STACKBIT_PHYS_DOT,
                        COLOR_HIGHLIGHT);
                }
            }
        }
        x += STACKBIT_PHYS_BLOCK_WIDTH + STACKBIT_PHYS_GAP;
    }

    draw_centered(tft_Ubuntu16, digits, STACKBIT_PHYS_NUMBER_Y);
    draw_centered(tft_DefaultFont, word, STACKBIT_PHYS_WORD_Y);
    draw_centered(tft_DefaultFont, footer, STACKBIT_PHYS_FOOTER_Y);
}

/* Geometry for a square, height-bound grid of `modules` cells against the right
 * edge, with the title column to its left: a full code with its quiet zone, or
 * a zoomed region without one. */
typedef struct {
    int scale;
    int extent;
    int top;
    int code_x;
    int title_x;
    int title_width;
} qr_geometry_t;

/* The code sits against the right edge, and the title and the way out share the
 * margin to its left. */
static qr_geometry_t qr_geometry(const int modules)
{
    qr_geometry_t g;
    g.scale = SEEDTOOL_DISPLAY_HEIGHT / modules;
    g.extent = modules * g.scale;
    g.top = (SEEDTOOL_DISPLAY_HEIGHT - g.extent) / 2;
    g.code_x = SEEDTOOL_DISPLAY_WIDTH - QR_MARGIN - g.extent;
    g.title_x = QR_MARGIN;
    g.title_width = g.code_x - 2 * QR_MARGIN;
    return g;
}

/* The way out, drawn the way the header's controls are: filled when it is
 * where the cursor sits, outlined when it is not. */
static void draw_qr_back(const bool selected)
{
    /* Painted either way, not only when selected: a screen that redraws a
     * control to move the cursor off it has to cover the highlight it had. */
    fill_rect(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_BLACK);
    draw_border(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_DIM);
    draw_triangle(NAV_BACK_X + (NAV_BACK_WIDTH - NAV_ARROW_WIDTH) / 2,
        NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_ARROW_HEIGHT) / 2, NAV_ARROW_WIDTH, NAV_ARROW_HEIGHT, TRIANGLE_LEFT,
        selected ? COLOR_BLACK : COLOR_WHITE);
}

/* The shade control, against the code's left edge, since what it changes is the
 * code. A sun whose disc is drawn in the current shade, so it dims as the code
 * does; the rays and outline stay in ink so it stays visible. Drawn from
 * rectangles: the faces are ASCII only. */
#define QR_SHADE_GAP 3
#define QR_SUN_CORE 5
#define QR_SUN_GAP 2
#define QR_SUN_RAY 2
#define QR_SUN_EXTENT (QR_SUN_CORE + 2 * (QR_SUN_GAP + QR_SUN_RAY))

/* A code is never wider than the display is tall, so its left edge never comes
 * further left than this, and the sun stays clear of the way out. */
_Static_assert(SEEDTOOL_DISPLAY_WIDTH - QR_MARGIN - SEEDTOOL_DISPLAY_HEIGHT - QR_SHADE_GAP - NAV_BACK_WIDTH
        > NAV_BACK_X + NAV_BACK_WIDTH,
    "the QR's shade control would overlap its back arrow");

static void draw_qr_shade(const int box_x, const bool selected)
{
    fill_rect(box_x, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_BLACK);
    draw_border(box_x, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_DIM);

    const uint16_t ink = selected ? COLOR_BLACK : COLOR_WHITE;
    const int left = box_x + (NAV_BACK_WIDTH - QR_SUN_EXTENT) / 2;
    const int top = NAV_BACK_Y + (NAV_BACK_HEIGHT - QR_SUN_EXTENT) / 2;
    const int core_x = left + QR_SUN_GAP + QR_SUN_RAY;
    const int core_y = top + QR_SUN_GAP + QR_SUN_RAY;
    fill_rect(core_x, core_y, QR_SUN_CORE, QR_SUN_CORE, qr_light());
    draw_border(core_x - 1, core_y - 1, QR_SUN_CORE + 2, QR_SUN_CORE + 2, ink);

    /* One ray per side, each starting a gap out from the disc's outline. */
    const int middle = core_x + QR_SUN_CORE / 2;
    const int centre = core_y + QR_SUN_CORE / 2;
    fill_rect(middle, top, 1, QR_SUN_RAY, ink);
    fill_rect(middle, top + QR_SUN_EXTENT - QR_SUN_RAY, 1, QR_SUN_RAY, ink);
    fill_rect(left, centre, QR_SUN_RAY, 1, ink);
    fill_rect(left + QR_SUN_EXTENT - QR_SUN_RAY, centre, QR_SUN_RAY, 1, ink);
    /* And a dot on each diagonal, which is what stops four rays reading as a
     * cross rather than as a sun. */
    fill_rect(left + 1, top + 1, 1, 1, ink);
    fill_rect(left + QR_SUN_EXTENT - 2, top + 1, 1, 1, ink);
    fill_rect(left + 1, top + QR_SUN_EXTENT - 2, 1, 1, ink);
    fill_rect(left + QR_SUN_EXTENT - 2, top + QR_SUN_EXTENT - 2, 1, 1, ink);
}

/* Every QR screen has the same two controls: the way out at the top left and
 * the sun against the code. */
static void draw_qr_chrome(const int code_x, const size_t selected)
{
    draw_qr_back(selected == SEEDTOOL_NAV_BACK);
    draw_qr_shade(code_x - QR_SHADE_GAP - NAV_BACK_WIDTH, selected == SEEDTOOL_NAV_SHADE);
}

/* The code against the right edge inside its quiet zone, on a cleared screen:
 * the light field in the current shade, the dark modules over it. The scale is
 * the largest that fits the display's height. */
static qr_geometry_t draw_modules(const QRCode* qr)
{
    const qr_geometry_t g = qr_geometry(qr->size + 2);
    seedtool_render_clear();
    fill_rect(g.code_x, g.top, g.extent, g.extent, qr_light());
    for (uint8_t y = 0; y < qr->size; ++y) {
        for (uint8_t x = 0; x < qr->size; ++x) {
            if (qrcode_getModule((QRCode*)qr, x, y)) {
                fill_rect(g.code_x + (x + 1) * g.scale, g.top + (y + 1) * g.scale, g.scale, g.scale, COLOR_BLACK);
            }
        }
    }
    return g;
}

/* Draws the populated `qr` with its title in the margin, so a stepped sequence
 * of codes says what each holds, then zeroes `modules`, which held the encoded
 * value. */
static bool draw_qr(QRCode* qr, uint8_t* modules, const char* title, const size_t selected)
{
    const qr_geometry_t g = draw_modules(qr);
    draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, QR_TITLE_Y);
    draw_qr_chrome(g.code_x, selected);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
    return true;
}

bool seedtool_render_qr(const char* title, const char* text, const size_t selected)
{
    /* At the smallest version that holds `text`, so a short value gets coarser,
     * easier modules. */
    const uint8_t version = qrcode_versionForText(ECC_LOW, text, QR_VERSION);
    if (!version) {
        return false;
    }
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (qrcode_initText(&qr, modules, version, ECC_LOW, text) != 0) {
        /* A failed encode may already have written part of `text` into
         * `modules` before the encoder gave up. */
        memset(modules, 0, sizeof(modules));
        return false;
    }
    return draw_qr(&qr, modules, title, selected);
}

/* Two groups to a line, so an address always falls into the same block; a
 * 42-character bech32 address is five full lines and a short sixth. */
#define QR_ADDRESS_GROUPS 2

/* The code, its derivation path and the address, on one screen: the way out,
 * then the path, then the address in groups of four, in the small face since it
 * is there to be checked, not transcribed. */
bool seedtool_render_qr_address(const char* title, const char* text, const size_t selected)
{
    const uint8_t version = qrcode_versionForText(ECC_LOW, text, QR_VERSION);
    if (!version) {
        return false;
    }
    const qr_geometry_t g = qr_geometry(17 + 4 * version + 2);

    const int text_top = NAV_BACK_Y + NAV_BACK_HEIGHT + 6 + 2 * tft_DefaultFont[1] + 2;
    const size_t total = strlen(text);

    /* A grid, not centred lines: the face is proportional, so each column is as
     * wide as its widest group and every group starts at its column's left
     * edge. */
    int column = 0;
    for (size_t i = 0; i < total; i += GROUP_LEN) {
        const size_t run = total - i < GROUP_LEN ? total - i : GROUP_LEN;
        int width = 0;
        for (size_t j = 0; j < run; ++j) {
            width += glyph_advance(tft_DefaultFont, (unsigned char)text[i + j]);
        }
        if (width > column) {
            column = width;
        }
    }
    const int block_width = QR_ADDRESS_GROUPS * column + (QR_ADDRESS_GROUPS - 1) * GROUP_GAP;

    /* Checked before anything is drawn, and why this returns a bool: an address
     * cut off at the margin looks like one that ended there. A taproot address
     * (62 characters) does not fit the 48 two columns hold. */
    const int step = tft_DefaultFont[1] + 2;
    const size_t lines = (total + QR_ADDRESS_GROUPS * GROUP_LEN - 1) / (QR_ADDRESS_GROUPS * GROUP_LEN);
    if (block_width > g.title_width || text_top + (int)lines * step > SEEDTOOL_DISPLAY_HEIGHT) {
        return false;
    }

    if (!seedtool_render_qr(NULL, text, selected)) {
        return false;
    }
    int y = NAV_BACK_Y + NAV_BACK_HEIGHT + 6;
    /* A blank line between the path and the value: they are two different
     * facts, and run together they read as one wrapped string. */
    y = draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, y) + tft_DefaultFont[1] + 2;

    int left = g.title_x + (g.title_width - block_width) / 2;
    if (left < g.title_x) {
        left = g.title_x;
    }

    for (size_t i = 0; i < total; i += GROUP_LEN) {
        const size_t run = total - i < GROUP_LEN ? total - i : GROUP_LEN;
        const size_t slot = (i / GROUP_LEN) % QR_ADDRESS_GROUPS;
        /* A last line that does not fill its columns is centred on the block. */
        const bool last_line = i + GROUP_LEN >= total && slot == 0;
        int x;
        if (last_line) {
            int width = 0;
            for (size_t j = 0; j < run; ++j) {
                width += glyph_advance(tft_DefaultFont, (unsigned char)text[i + j]);
            }
            x = left + (block_width - width) / 2;
        } else {
            x = left + (int)slot * (column + GROUP_GAP);
        }
        const uint16_t ink = group_ink(i / GROUP_LEN);
        for (size_t j = 0; j < run; ++j) {
            draw_glyph(tft_DefaultFont, (unsigned char)text[i + j], x, y, ink);
            x += glyph_advance(tft_DefaultFont, (unsigned char)text[i + j]);
        }
        if (slot + 1 == QR_ADDRESS_GROUPS) {
            y += tft_DefaultFont[1] + 2;
        }
    }
    return true;
}

size_t seedtool_render_qr_alphanumeric_capacity(const uint8_t max_version)
{
    size_t chars = 0;
    while (qrcode_versionForAlphanumeric(ECC_LOW, (uint16_t)(chars + 1), max_version)) {
        ++chars;
    }
    return chars;
}

/* Encodes `data` in byte mode at the smallest version that holds it: that is
 * what "compact" means in SeedSigner's convention. `modules` must hold
 * qrcode_getBufferSize(QR_VERSION), which bounds every smaller version. */
static bool encode_bytes(QRCode* qr, uint8_t* modules, const uint8_t* data, const size_t len)
{
    if (len > UINT16_MAX) {
        return false;
    }
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    /* qrcode_initBytes only reads through `data`; the cast does not let it
     * write through it. */
    if (!version || qrcode_initBytes(qr, modules, version, ECC_LOW, (uint8_t*)data, (uint16_t)len) != 0) {
        /* `data` is seed entropy, and a failed encode may already have written
         * some of it into `modules`. */
        memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
        return false;
    }
    return true;
}

bool seedtool_render_qr_bytes(const char* title, const uint8_t* data, const size_t len, const size_t selected)
{
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    return encode_bytes(&qr, modules, data, len) && draw_qr(&qr, modules, title, selected);
}

/* 7x7 blocks for the smallest (version 1, 21x21) QR, 5x5 otherwise: Krux's own
 * thresholds (src/krux/pages/qr_view.py, SeedQRView.__init__). */
static size_t qr_region_size(const uint8_t qr_size) { return qr_size == 21 ? 7 : 5; }

size_t seedtool_render_qr_bytes_regions(const size_t len)
{
    if (len > UINT16_MAX) {
        return 0;
    }
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    if (!version) {
        return 0;
    }
    const uint8_t qr_size = (uint8_t)(4 * version + 17);
    const size_t region_size = qr_region_size(qr_size);
    const size_t columns = ((size_t)qr_size + region_size - 1) / region_size;
    return columns * columns;
}

/* The region map: seedtool_render_qr_bytes's code, same geometry, with a dim
 * line at each region boundary and each region's label inside it, from the same
 * split draw_qr_region uses, so the map always matches the tiles. */
bool seedtool_render_qr_bytes_map(const char* title, const uint8_t* data, const size_t len, const size_t selected)
{
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (!encode_bytes(&qr, modules, data, len)) {
        return false;
    }
    const size_t region_size = qr_region_size(qr.size);
    const size_t columns = ((size_t)qr.size + region_size - 1) / region_size;
    const qr_geometry_t g = draw_modules(&qr);
    /* The code's top-left corner, one quiet-zone module in, which the grid and
     * labels are placed from. */
    const int code_left = g.code_x + g.scale;
    const int code_top = g.top + g.scale;
    const int code_extent = (int)qr.size * g.scale;
    const int block = (int)region_size * g.scale;
    for (size_t i = 0; i <= columns; ++i) {
        const int pos = (int)i * block;
        fill_rect(code_left, code_top + pos, code_extent, 1, COLOR_DIM);
        fill_rect(code_left + pos, code_top, 1, code_extent, COLOR_DIM);
    }
    /* A label must read over black and white modules alike, so each gets a
     * small white plate with black text. */
    for (size_t row = 0; row < columns; ++row) {
        for (size_t column = 0; column < columns; ++column) {
            char label[4];
            (void)snprintf(label, sizeof(label), "%c%u", (char)('A' + row), (unsigned)(column + 1));
            const int label_x = code_left + (int)column * block;
            const int label_y = code_top + (int)row * block + 2;
            const int label_height = tft_DefaultFont[1] + 2;
            fill_rect(label_x + 1, label_y - 1, block - 2, label_height, COLOR_WHITE);
            draw_centered_in(tft_DefaultFont, label, label_x, block, label_y, COLOR_BLACK);
        }
    }
    draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, QR_TITLE_Y);
    draw_qr_chrome(g.code_x, selected);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
    return true;
}

/* draw_qr's layout at a region's own scale: 5-7 cells rather than 21-25, so
 * each is much larger. No quiet zone: a region is copied by hand, never
 * scanned. */
static void draw_qr_region(QRCode* qr, uint8_t* modules, const char* title, const size_t region_size,
    const size_t columns, const size_t region_index, const size_t selected)
{
    const size_t row = region_index / columns;
    const size_t column = region_index % columns;
    const qr_geometry_t g = qr_geometry((int)region_size);
    seedtool_render_clear();
    for (size_t y = 0; y < region_size; ++y) {
        for (size_t x = 0; x < region_size; ++x) {
            const size_t qx = column * region_size + x;
            const size_t qy = row * region_size + y;
            const bool set = qx < qr->size && qy < qr->size && qrcode_getModule(qr, (uint8_t)qx, (uint8_t)qy);
            fill_rect(g.code_x + (int)x * g.scale, g.top + (int)y * g.scale, g.scale, g.scale,
                set ? COLOR_BLACK : qr_light());
        }
    }
    for (size_t i = 0; i <= region_size; ++i) {
        fill_rect(g.code_x, g.top + (int)i * g.scale, g.extent, 1, COLOR_DIM);
        fill_rect(g.code_x + (int)i * g.scale, g.top, 1, g.extent, COLOR_DIM);
    }
    const int label_y = draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, QR_TITLE_Y);
    char label[24];
    (void)snprintf(label, sizeof(label), "Region %c%u", (char)('A' + row), (unsigned)(column + 1));
    draw_centered_box(tft_DefaultFont, label, g.title_x, g.title_width, label_y + 4);
    draw_qr_chrome(g.code_x, selected);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
}

bool seedtool_render_qr_bytes_region(
    const char* title, const uint8_t* data, const size_t len, const size_t region_index, const size_t selected)
{
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (!encode_bytes(&qr, modules, data, len)) {
        return false;
    }
    const size_t region_size = qr_region_size(qr.size);
    const size_t columns = ((size_t)qr.size + region_size - 1) / region_size;
    if (region_index >= columns * columns) {
        memset(modules, 0, sizeof(modules));
        return false;
    }
    draw_qr_region(&qr, modules, title, region_size, columns, region_index, selected);
    return true;
}

const uint16_t* seedtool_render_pixels(void) { return framebuffer; }

void seedtool_render_wire_rows(uint16_t* const out, const size_t first_row, const size_t rows)
{
    const size_t count = rows * SEEDTOOL_DISPLAY_WIDTH;
    const uint16_t* const pixels = framebuffer + first_row * SEEDTOOL_DISPLAY_WIDTH;
    for (size_t i = 0; i < count; ++i) {
        out[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
    }
}
