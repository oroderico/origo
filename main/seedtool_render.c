/* The renderer's core: the framebuffer, text, the nav chrome, lists and the
 * plain screens, and the primitives render_entry.c and render_qr.c draw with
 * (render_internal.h). */
#include "render_internal.h"

#include "seedtool_logo.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Left margin for the four-row body: a numbered list reads as a table, so every
 * row starts at the same x. */
#define SCREEN4_TEXT_X 20

/* The 16px title, three 33px rows and the 11px footer fill the 135px display.
 * The scroll strip on the right is kept clear of the selection bar. */
#define LIST_TITLE_Y 3
#define LIST_TOP 21

#define LIST_GUTTER 10
#define LIST_BAR_X 2
#define LIST_BAR_WIDTH (SEEDTOOL_DISPLAY_WIDTH - LIST_GUTTER - 2 * LIST_BAR_X)
#define LIST_TEXT_X 6
#define LIST_TEXT_WIDTH (LIST_BAR_X + LIST_BAR_WIDTH - LIST_TEXT_X)
#define LIST_SCROLL_X (SEEDTOOL_DISPLAY_WIDTH - 7)
#define LIST_SCROLL_WIDTH 4
#define LIST_SCROLL_MIN 10

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

typedef struct {
    uint8_t y_offset;
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    uint8_t x_advance;
    const uint8_t* bitmap;
} glyph_t;

_Alignas(4) static uint16_t framebuffer[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];

void fill_rect(int x, int y, int width, int height, const uint16_t color)
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

/* The widest paged or tailed line: the panel less a margin either side, so a
 * line of a key or an address does not run edge to edge. */
#define TEXT_MARGIN 6
#define TEXT_WIDTH (SEEDTOOL_DISPLAY_WIDTH - 2 * TEXT_MARGIN)

int glyph_advance(const uint8_t* font, const unsigned char character)
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

void draw_glyph(
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

void draw_line_at(
    const uint8_t* font, const char* text, const size_t length, int x, const int y, const uint16_t color)
{
    for (size_t i = 0; i < length; ++i) {
        draw_glyph(font, (unsigned char)text[i], x, y, color);
        x += glyph_advance(font, (unsigned char)text[i]);
    }
}

/* Centre `text` inside the box of `width` pixels starting at `x`. */
void draw_centered_in(const uint8_t* font, const char* text, const int x, const int width, const int y,
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
/* Lays `text` out centred in a box `width` wide from row `y`, wrapping as it
 * goes, and returns the row below its last line. With `draw` false it only
 * measures, so a screen can place what follows before drawing anything. */
int layout_centered_box(
    const uint8_t* font, const char* text, const int x, const int width, int y, const bool draw)
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
        if (draw) {
            draw_centered_line(font, cursor, (size_t)(line_end - cursor), x, width, y);
        }
        cursor = *line_end == ' ' || *line_end == '\n' ? line_end + 1 : line_end;
        y += line_height;
    }
    return y;
}

int draw_centered_box(const uint8_t* font, const char* text, const int x, const int width, const int y)
{
    return layout_centered_box(font, text, x, width, y, true);
}

/* The lowest row any glyph of `text` inks, from the top of its line: a font's
 * declared height leaves out descenders. */
int text_ink_bottom(const uint8_t* font, const char* text, const size_t length)
{
    int bottom = 0;
    for (size_t i = 0; i < length; ++i) {
        glyph_t glyph;
        if (find_glyph(font, (unsigned char)text[i], &glyph) && glyph.y_offset + glyph.height > bottom) {
            bottom = glyph.y_offset + glyph.height;
        }
    }
    return bottom;
}

void draw_centered(const uint8_t* font, const char* text, const int y)
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
void draw_typed(const uint8_t* font, const char* text, const int y)
{
    const size_t length = strlen(text);
    size_t start = 0;
    while (start < length && text_width(font, text + start, length - start) > TEXT_WIDTH) {
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
    return fit_in_wrapped(FONT_BODY, text, limit, TEXT_WIDTH);
}

/* Ink for the group at `index`, counted from the start of the whole value so
 * the alternation carries across a line break rather than restarting. */
uint16_t group_ink(const size_t index) { return index % 2 ? COLOR_HIGHLIGHT : COLOR_WHITE; }

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
    return fit_grouped_in(FONT_BODY, text, limit, TEXT_WIDTH);
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

size_t seedtool_render_fit_row(const char* text) { return fit_in(FONT_BODY, text, SIZE_MAX, LIST_TEXT_WIDTH); }

size_t seedtool_render_fit_tail(const char* text)
{
    const size_t length = strlen(text);
    size_t start = 0;
    while (start < length && text_width(FONT_BODY, text + start, length - start) > TEXT_WIDTH) {
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
    draw_centered(FONT_TITLE, title, 5);
    /* The body carries the values that get transcribed, so it takes the larger
     * face. The footer stays small or it would run off the display. */
    draw_centered(FONT_BODY, line1, 39);
    draw_centered(FONT_BODY, line2, 65);
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
void draw_triangle(
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

void draw_border(const int x, const int y, const int width, const int height, const uint16_t color)
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
     * does not paint into it. A title too long for the header face on one line
     * (a key origin under a three-digit account) takes the smaller face, since
     * a second line would land on the body. */
    const int title_width = SEEDTOOL_DISPLAY_WIDTH - 2 * inset;
    if (title && nav->fingerprint) {
        /* The label in the title face, the fingerprint after it as content.
         * Both are short enough for the line: "Wallet @" and eight hex. */
        const int label_width = text_width(FONT_TITLE, title, strlen(title));
        const int value_width = text_width(FONT_BODY, nav->fingerprint, strlen(nav->fingerprint));
        const int x = inset + (title_width - label_width - value_width) / 2;
        draw_line_at(FONT_TITLE, title, strlen(title), x, LIST_TITLE_Y, COLOR_WHITE);
        draw_line_at(FONT_BODY, nav->fingerprint, strlen(nav->fingerprint), x + label_width, LIST_TITLE_Y,
            COLOR_FINGERPRINT);
    } else if (title && text_width(FONT_TITLE, title, strlen(title)) > title_width) {
        (void)draw_centered_box(tft_DefaultFont, title, inset, title_width,
            NAV_BACK_Y + (NAV_BACK_HEIGHT - tft_DefaultFont[1]) / 2);
    } else {
        (void)draw_centered_box(FONT_TITLE, title, inset, title_width, LIST_TITLE_Y);
    }
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
void nav_begin(const seedtool_nav_t* nav, const char* title)
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
        draw_centered(FONT_BODY, line1, 33);
        draw_centered(FONT_BODY, line2, 58);
        draw_centered(FONT_BODY, line3, 83);
    } else {
        draw_centered(FONT_BODY, line1, 39);
        draw_centered(FONT_BODY, line2, 65);
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
                FONT_BODY, lines[i], strlen(lines[i]), 0, SEEDTOOL_DISPLAY_WIDTH, y[i], first_group[i]);
        }
    }
    nav_end(nav);
}

void seedtool_render_nav_rows(
    const seedtool_nav_t* nav, const char* title, const char* const* rows, const size_t count)
{
    nav_begin(nav, title);
    for (size_t row = 0; row < NAV_ROWS_SHOWN && row < count; ++row) {
        draw_left(FONT_BODY, rows[row], SCREEN4_TEXT_X, NAV_ROWS_TOP + (int)row * NAV_ROWS_HEIGHT);
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
        draw_row(FONT_BODY, item, LIST_TEXT_X, y + (NAV_ROW_HEIGHT - FONT_BODY[1]) / 2, LIST_TEXT_WIDTH,
            highlighted ? COLOR_BLACK : COLOR_WHITE);
    }
    if (count > SEEDTOOL_LIST_ROWS) {
        const seedtool_thumb_t thumb = seedtool_list_thumb(count, top, NAV_SCROLL_HEIGHT);
        fill_rect(LIST_SCROLL_X, LIST_TOP, LIST_SCROLL_WIDTH, NAV_SCROLL_HEIGHT, COLOR_DIM);
        fill_rect(LIST_SCROLL_X, LIST_TOP + thumb.offset, LIST_SCROLL_WIDTH, thumb.height, COLOR_WHITE);
    }
    nav_end(nav);
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
