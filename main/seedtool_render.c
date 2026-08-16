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

/* Sits in the gap between the transcript line (ends y=81) and the footer
 * (starts y=111) that seedtool_render_screen leaves empty on a dice-entry
 * screen. */
#define DICE_BAR_MARGIN 20
#define DICE_BAR_X DICE_BAR_MARGIN
#define DICE_BAR_WIDTH (SEEDTOOL_DISPLAY_WIDTH - 2 * DICE_BAR_MARGIN)
#define DICE_BAR_Y 90
#define DICE_BAR_HEIGHT 14

/* Left margin for the four-row body: a numbered list reads as a table, so every
 * row starts at the same x rather than each being centred as its own block and
 * drifting with how wide its own text is. */
#define SCREEN4_TEXT_X 20

#define KEYBOARD_COLUMNS 10
#define KEY_WIDTH (SEEDTOOL_DISPLAY_WIDTH / KEYBOARD_COLUMNS)
#define KEY_HEIGHT 27
#define KEYBOARD_TOP (SEEDTOOL_DISPLAY_HEIGHT - 3 * KEY_HEIGHT)

/* The 6px gap left between the typed text (ends y=48) and the keyboard
 * (starts at KEYBOARD_TOP=54) - same DICE_BAR_X/WIDTH margin as the entropy
 * bar, so a word-count progress bar reads as the same widget in a narrower
 * space, not a new one. */
#define WORD_BAR_Y (KEYBOARD_TOP - 4)
#define WORD_BAR_HEIGHT 3

/* The 16px title, three 33px rows and the 11px footer fill the 135px display
 * exactly. The rightmost strip carries the scroll arrows and is kept clear of
 * the selection bar so a long label never runs underneath one. */
#define LIST_TITLE_Y 3
#define LIST_TOP 21
#define LIST_ROW_HEIGHT 33
#define LIST_FOOTER_Y 122

/* The plain screens put their title at the top and their footer near the
 * bottom; the band between is what the screen is for. Named because the digit
 * field centres itself in it. */
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
#define LIST_SCROLL_HEIGHT (SEEDTOOL_LIST_ROWS * LIST_ROW_HEIGHT)
#define LIST_SCROLL_MIN 10

/* Nav chrome. The back arrow lives inside the title bar, so the rows below it
 * keep the full height they have on a plain list bar one pixel each, which is
 * what pays for the confirm bar at the bottom. The title is centred between
 * two margins of NAV_BACK_WIDTH rather than across the whole screen, so the
 * arrow does not push it visually off-centre. */
#define NAV_BACK_X 2
#define NAV_BACK_Y 1
#define NAV_BACK_WIDTH 20
#define NAV_BACK_HEIGHT 19
#define NAV_ARROW_WIDTH 7
#define NAV_ARROW_HEIGHT 11
/* The tick gets its own box rather than the arrow's: the arrow is tall and
 * narrow to read as a triangle, a check this small has to be wider than tall.
 * The legs and stroke are the shape; the box is what they add up to, derived
 * so it cannot drift from them. */
#define NAV_TICK_STROKE 2
#define NAV_TICK_SHORT 3
#define NAV_TICK_LONG 6
#define NAV_TICK_WIDTH (NAV_TICK_SHORT + NAV_TICK_LONG + NAV_TICK_STROKE)
#define NAV_TICK_HEIGHT (NAV_TICK_LONG + NAV_TICK_STROKE)
#define NAV_ROW_HEIGHT 32
/* The rows' own height, less the 2px the last one leaves under itself: a
 * plain list can let its track overhang that gap, having nothing below it,
 * but here the confirm bar is what comes next and the two must not touch. */
#define NAV_SCROLL_HEIGHT (SEEDTOOL_LIST_ROWS * NAV_ROW_HEIGHT - 2)
#define NAV_BAR_Y 118
#define NAV_BAR_HEIGHT 17
/* Between the lowest body line a paged screen draws (screen4's fourth, ending
 * at 103) and the bar. The small face fits the 14px left over; the 16px one
 * would not. */
#define NAV_COUNTER_Y 105
/* Four left-aligned rows. The heights are inherited from the plain four-line
 * screen this replaced, and kept rather than re-derived: 28 to 88 in steps of
 * 20 fits four 16px lines between the header and the counter with a real gap
 * under each, which is the arithmetic the pixel scan in the self-test holds
 * them to. */
#define NAV_ROWS_SHOWN 4
#define NAV_ROWS_TOP 28
#define NAV_ROWS_HEIGHT 20

/* Version 6 holds 134 bytes at ECC_LOW, enough for a key origin and an account
 * xpub in one image. Raising it again is a compile error rather than a code
 * quietly clipped by the bottom of the display. */
#define QR_VERSION 6
#define QR_MAX_MODULES (17 + 4 * QR_VERSION)
#define QR_LEFT 3
#define QR_TITLE_Y 50
_Static_assert(SEEDTOOL_DISPLAY_HEIGHT / (QR_MAX_MODULES + 2) >= 1, "the largest QR no longer fits the display");

/* Stackbit 1248 punch grid: four digit columns (thousands..units, left to
 * right) times four weight rows (1,2,4,8 top to bottom), with the word
 * number and word text in a panel to its right. A simplified layout rather
 * than a pixel-faithful copy of the physical plate's own 2-row arrangement:
 * read by weight label, not by matching screen geometry to the plate. */
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

/* The physical Stackbit 1248 plate's own arrangement (github.com/selfcustody/krux,
 * src/krux/pages/stack_1248.py: _draw_grid/_draw_punched): two rows, not four.
 * The thousands digit gets one column of two cells (weight 1 on top, weight 2
 * below — the digit is never more than 2, so only one is ever lit); the other
 * three digits each get a 2x2 block, top-left=1, top-right=2, bottom-left=4,
 * bottom-right=8. Below the grid rather than beside it, since it is wide and
 * short rather than tall and narrow. */
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

/* Centred and wrapped inside the box of `width` pixels starting at `x`. A
 * wrap that would land inside a word backs off to the last space instead, so
 * the word carries whole onto the next line; a single word wider than the
 * whole box is the one case with no better break, and is left to split.
 * Returns the y just past the last line drawn, so a caller stacking more text
 * beneath a value of unknown line count - draw_qr_region's title, which wraps
 * whenever the zoomed layout's narrow column can't fit it on one line - can
 * place it without guessing how many lines came before it. */
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
        int used = 0;
        while (*end && *end != '\n') {
            const int advance = glyph_advance(font, (unsigned char)*end);
            if (end != cursor && used + advance > width) {
                break;
            }
            if (*end == ' ') {
                last_space = end;
            }
            used += advance;
            ++end;
        }
        const bool mid_word = end > cursor && end[-1] != ' ' && *end && *end != '\n' && *end != ' ';
        const char* const line_end = mid_word && last_space && last_space != cursor ? last_space : end;
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

/* Left-aligned rather than boxed and centred: a numbered list reads as a
 * table, with the word (or word number) starting at the same x on every row,
 * not drifting with how wide each entry's own text happens to be. */
static void draw_left(const uint8_t* font, const char* text, const int x, const int y)
{
    draw_line_at(font, text, strlen(text), x, y, COLOR_WHITE);
}

/* What has been typed so far, read back to be checked, so it takes the larger
 * face. Only the tail that fits one line is drawn: a passphrase long enough to
 * wrap would push a second line down into the keyboard. */
static void draw_typed(const uint8_t* font, const char* text, const int y)
{
    const size_t length = strlen(text);
    size_t start = 0;
    while (start < length && text_width(font, text + start, length - start) > SEEDTOOL_DISPLAY_WIDTH - 4) {
        ++start;
    }
    draw_centered_line(font, text + start, length - start, 0, SEEDTOOL_DISPLAY_WIDTH, y);
}

/* Leading characters of `text` that fit in `max_width` pixels, at most `limit`
 * of them. The body font is proportional, so every caller that has to know where
 * text stops measures it here rather than counting characters. */
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

/* Same as fit_in, but backs a mid-word fit off to the last space within it,
 * so paged text (page_text, via seedtool_render_fit) never splits a word
 * across pages the way a raw pixel-width cut can. Only for callers that have
 * a next line to carry the rest of the word onto: a list row's single-line
 * truncation (seedtool_render_fit_row) has none, so it keeps the plain cut. */
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

/* A Bitcoin address is a single unbroken run of base58 or bech32, which is
 * exactly the shape a reader loses their place in: no word boundaries, no
 * repeated shapes to count by, and characters chosen to look unlike each
 * other rather than to group. Drawn in fours with a gap and an alternating
 * ink, the eye gets a place to rest every four characters and two independent
 * signals for where one group ends - the space and the colour change - so
 * neither carries the grouping alone.
 *
 * The address itself is never touched. Grouping happens at draw time only:
 * what is compared, hashed and put in a QR is the same unbroken string it
 * always was, and nothing downstream has to know to strip separators back
 * out. */
#define GROUP_LEN SEEDTOOL_GROUP_LEN
#define GROUP_GAP 5

/* Ink for the group at `index`, counted from the start of the whole value so
 * the alternation carries across a line break rather than restarting. */
static uint16_t group_ink(const size_t index) { return index % 2 ? COLOR_HIGHLIGHT : COLOR_WHITE; }

/* `length` characters of `text` drawn in groups, centred on the line as a
 * whole. `first_group` is how many groups came before this line. */
static void draw_grouped(
    const uint8_t* font, const char* text, const size_t length, const int y, const size_t first_group)
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
    int x = (SEEDTOOL_DISPLAY_WIDTH - width) / 2;
    if (x < 0) {
        x = 0;
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

/* How many characters of `text` fit one line once the gaps are paid for.
 * Rounded down to a whole number of groups, so a group is never split across
 * a line and the alternation stays readable at the break - unless the value
 * ends mid-group, which is the one short group allowed. */
size_t seedtool_render_fit_grouped(const char* text, const size_t limit)
{
    const int max_width = SEEDTOOL_DISPLAY_WIDTH - 4;
    int width = 0;
    size_t count = 0;
    for (; count < limit && text[count]; ++count) {
        int advance = glyph_advance(tft_Ubuntu16, (unsigned char)text[count]);
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


/* A row's text, with an ellipsis when it did not all fit. A row that simply
 * stops is indistinguishable from a row that ended - which is fine for a menu
 * label, since those are held to fitting by a self-test, and wrong for the
 * address list, where every row is a value cut short and the reader has no
 * way to tell how much is missing. The three dots say "there is more" without
 * claiming how much; the address's own page says the rest.
 *
 * The ellipsis is paid for out of the same width, not drawn past it: the fit
 * is recomputed against what is left after reserving room for the dots, so a
 * truncated row is never wider than one that fits. */
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

/* The splash is the logo as it was drawn — mark, wordmark and tagline in one
 * picture — and nothing else: it holds the screen briefly and then the menu
 * takes over, so there is no instruction to give. */
_Static_assert(SEEDTOOL_LOGO_WIDTH <= SEEDTOOL_DISPLAY_WIDTH, "the logo is wider than the display");
_Static_assert(SEEDTOOL_LOGO_HEIGHT <= SEEDTOOL_DISPLAY_HEIGHT, "the logo is taller than the display");

/* Sixteen palette entries at two pixels per byte, high nibble first. Written
 * straight into the framebuffer: it is one picture on one screen, not a drawing
 * surface for anything else to use. */
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
     * face. It costs four characters a line and no extra page: this font is 45%
     * taller than the small one but only 18% wider on base58. The footer stays
     * small because 16px there would run off the bottom of the display. */
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
    /* Spread the remaining travel over the scrollable range, so the thumb sits
     * against the top at the first row and flush with the bottom at the last:
     * the end of a list has to look like the end of it here too. On a long
     * enough list the division below can floor a small top back down to 0,
     * which would show row 1 as if it were row 0; nudge it to the nearest
     * pixel off the top instead, the one case the floor can get wrong. */
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

/* A solid isoceles triangle filling a width x height box, apex pointing the
 * way asked - drawn rather than set, since the 16px face carries no glyph for
 * one and Jade's own symbols font is not linked here.
 *
 * Up and down walk the rows drawing horizontal bars; left walks the columns
 * drawing vertical ones. That is the same arithmetic transposed, which is why
 * the header's back arrow and the digit field's scroll hints are one function
 * rather than two that drift apart.
 *
 * The `+ 1` on the step is what keeps the apex a single pixel: without it the
 * first two steps round to the same half-width and the point comes out as a
 * two-pixel spike. */
typedef enum { TRIANGLE_UP, TRIANGLE_DOWN, TRIANGLE_LEFT } triangle_dir_t;

static void draw_triangle(
    const int x, const int y, const int width, const int height, const triangle_dir_t dir, const uint16_t color)
{
    const bool vertical = dir != TRIANGLE_LEFT;
    const int steps = vertical ? height : width;
    const int across = vertical ? width : height;
    for (int i = 0; i < steps; ++i) {
        const int from_point = dir == TRIANGLE_DOWN ? steps - 1 - i : i;
        const int half = (from_point + 1) * across / (2 * steps);
        if (vertical) {
            fill_rect(x + width / 2 - half, y + i, 2 * half + 1, 1, color);
        } else {
            fill_rect(x + i, y + height / 2 - half, 1, 2 * half + 1, color);
        }
    }
}

/* A tick, drawn rather than set, for the same reason the arrow above is: the
 * 16px face has no glyph for one and Jade's symbols font is a component the
 * audit rejects by name. Two strokes up from a common low point, each a stack
 * of squares so the diagonals read as solid rather than as dotted pixels.
 *
 * The legs are fixed lengths rather than fractions of one width - at this size
 * that rounds the short leg away entirely - and roughly 1:2 is what reads as a
 * check. `x, y` is the top-left of the NAV_TICK_WIDTH x NAV_TICK_HEIGHT box. */
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

/* The chrome itself, drawn the same way for every screen that wears it - the
 * point of the thing being that the arrow and the bar are found in one place
 * regardless of what sits between them. Split from the screens so a new one
 * inherits the geometry rather than restating it. */
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
        /* Sides and top, no bottom - Jade's own header buttons are bordered
         * that way on a full menu, its comment giving the reason: "bottom edge
         * will be covered by upper line above top menu item" (ui/dialogs.c).
         * The rule above the first row sits directly under this box, so the
         * two meet and the chrome reads as one frame rather than as a button
         * hovering above an unrelated list. Drawn whether or not the arrow is
         * selected, dim when idle: a bare arrow in the corner reads as
         * decoration, a box reads as something that can be taken. */
        const uint16_t edge = on_back ? COLOR_HIGHLIGHT : COLOR_DIM;
        fill_rect(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, 1, edge);
        fill_rect(NAV_BACK_X, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        fill_rect(NAV_BACK_X + NAV_BACK_WIDTH - 1, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        draw_triangle(NAV_BACK_X + (NAV_BACK_WIDTH - NAV_ARROW_WIDTH) / 2,
            NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_ARROW_HEIGHT) / 2, NAV_ARROW_WIDTH, NAV_ARROW_HEIGHT, TRIANGLE_LEFT,
            on_back ? COLOR_BLACK : COLOR_WHITE);
    }
    /* The confirm as a tick in the right slot, mirroring the arrow: same box,
     * same borders, same rule closing it underneath. The slot has been held
     * open by the title's own margins since the chrome arrived, so nothing
     * moves to make room for it. */
    /* Only when there is something to confirm. A menu passes no label because
     * its rows are its actions, and a tick there offers an answer to a
     * question the screen never asked - the same guard the bar had, which this
     * did not inherit when it replaced it. */
    if (nav->confirm_as_tick && nav->confirm) {
        /* Three states, the same three the bar had: filled when it is both
         * available and selected, outlined when available and not, and drawn
         * dim throughout when confirming is not possible yet - the checksum
         * screen with a word still wrong is the case that needs the third.
         * The control keeps its place in every state, so what changes is
         * whether it can be taken and never whether it is there. */
        const bool on_tick = nav->confirm_enabled && nav->selected == SEEDTOOL_NAV_CONFIRM;
        const int x = SEEDTOOL_DISPLAY_WIDTH - NAV_BACK_X - NAV_BACK_WIDTH;
        if (on_tick) {
            fill_rect(x, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, COLOR_HIGHLIGHT);
        }
        const uint16_t edge = on_tick ? COLOR_HIGHLIGHT : COLOR_DIM;
        fill_rect(x, NAV_BACK_Y, NAV_BACK_WIDTH, 1, edge);
        fill_rect(x, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        fill_rect(x + NAV_BACK_WIDTH - 1, NAV_BACK_Y, 1, NAV_BACK_HEIGHT, edge);
        draw_tick(x + (NAV_BACK_WIDTH - NAV_TICK_WIDTH) / 2, NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_TICK_HEIGHT) / 2,
            !nav->confirm_enabled ? COLOR_DIM : on_tick ? COLOR_BLACK : COLOR_WHITE);
    }
    /* Centred between two margins the width of the arrow's box, not across the
     * glass: a title centred over the whole width would read as leaning right
     * against the arrow, and a long one would paint into it. */
    (void)draw_centered_box(tft_Ubuntu16, title, inset, SEEDTOOL_DISPLAY_WIDTH - 2 * inset, LIST_TITLE_Y);
    /* The rule that closes the header, and with it the bottom of the arrow's
     * box and the tick's - both are drawn with sides and top only, on the
     * understanding that a line underneath would be the fourth edge. On a list
     * that line was the rule above the first cell, so the frame closed and the
     * chrome read as one piece; every other screen wearing this header had no
     * cells to supply it, so the two boxes hung open with nothing across the
     * bar. Drawn here instead of by each body, so a screen gets it for wearing
     * the header rather than for happening to be a list. */
    fill_rect(NAV_BACK_X, NAV_BACK_Y + NAV_BACK_HEIGHT, SEEDTOOL_DISPLAY_WIDTH - 2 * NAV_BACK_X, 1, COLOR_DIM);
}

static void draw_nav_bar(const seedtool_nav_t* nav)
{
    /* Nothing along the bottom when the confirm is a tick in the header: the
     * control exists once, and drawing it twice would make the reader look for
     * the difference between them. */
    if (!nav->confirm || nav->confirm_as_tick) {
        return;
    }
    const bool on_confirm = nav->selected == SEEDTOOL_NAV_CONFIRM;
    /* Filled when it is both selectable and selected, outlined when it is
     * selectable but not selected, dimmed when it cannot be taken at all - so
     * the bar is in the same place either way and only its state says whether
     * confirming is available yet. */
    const uint16_t ink = !nav->confirm_enabled ? COLOR_DIM : on_confirm ? COLOR_BLACK : COLOR_WHITE;
    if (on_confirm && nav->confirm_enabled) {
        fill_rect(0, NAV_BAR_Y, SEEDTOOL_DISPLAY_WIDTH, NAV_BAR_HEIGHT, COLOR_HIGHLIGHT);
    } else {
        fill_rect(0, NAV_BAR_Y, SEEDTOOL_DISPLAY_WIDTH, 1, nav->confirm_enabled ? COLOR_WHITE : COLOR_DIM);
    }
    draw_centered_in(tft_Ubuntu16, nav->confirm, 0, SEEDTOOL_DISPLAY_WIDTH,
        NAV_BAR_Y + (NAV_BAR_HEIGHT - tft_Ubuntu16[1]) / 2, ink);
}

/* The page counter, in the gap the body leaves above the confirm bar. Small
 * face: it is a position indicator, not a value to transcribe. */
static void draw_nav_counter(const seedtool_nav_t* nav)
{
    if (nav->counter) {
        draw_centered_in(tft_DefaultFont, nav->counter, 0, SEEDTOOL_DISPLAY_WIDTH, NAV_COUNTER_Y, COLOR_WHITE);
    }
}

/* Opens and closes every nav screen, so the order - clear, chrome, body,
 * counter, bar - is written once and a new screen type only has to say what
 * goes between them. */
static void nav_begin(const seedtool_nav_t* nav, const char* title)
{
    seedtool_render_clear();
    draw_nav_header(nav, title);
}

static void nav_end(const seedtool_nav_t* nav)
{
    draw_nav_counter(nav);
    draw_nav_bar(nav);
}

void seedtool_render_nav_text(
    const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2, const char* line3)
{
    nav_begin(nav, title);
    /* Two layouts in one function, chosen by whether there is a third line:
     * two lines sit at 39 and 65, three at 33, 58 and 83, pitched tighter to
     * clear the counter. These were two separate screens before the chrome,
     * and the heights are theirs unchanged - a screen gaining the chrome
     * should not also move its own text. */
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
    /* The three-line pitch, always: an address is long enough to want the
     * lines and short enough to fit them, and holding one layout keeps a
     * two-line page from moving its text when a third line appears. */
    static const int y[] = { 33, 58, 83 };
    for (size_t i = 0; i < count && i < 3; ++i) {
        if (lines[i] && lines[i][0]) {
            draw_grouped(tft_Ubuntu16, lines[i], strlen(lines[i]), y[i], first_group[i]);
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
        /* A rule above every row, the first included, so they read as cells
         * rather than as lines of text that happen to be stacked - Jade draws
         * a border under each menu item and above the top one (ui/dialogs.c).
         * It sits in the gap between cells, so the selection bar never paints
         * over it. The topmost one lands on the header's own closing rule and
         * repaints it in the same colour: that rule used to be this line's job,
         * until every screen wearing the header needed one and it moved there.
         * Kept because the cell wants a top edge whether or not the header drew
         * one. The last cell is closed by the confirm bar's own rule, so three
         * rules here and that one frame three rows. */
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

/* A backspace arrow, drawn rather than spelled: the fonts are 95 printable ASCII
 * characters and have no glyph for it, and the word "del" needed the small face
 * to fit a key the letters had already outgrown. */
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

/* The dice-roll quality bar, drawn wherever a screen leaves room for it: the
 * plain dice screen, the digit field and the nav chrome's dice screen all sit
 * above DICE_BAR_Y. The NULL guard is here rather than at each caller because
 * three of them pass a bar that a screen may or may not want. */
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

/* A row of digit boxes, one per character position, with the one being set
 * carrying an up and a down arrow: the value scrolls in place instead of being
 * hunted for on a keypad. Adapted from Blockstream Jade's PIN and index entry
 * (main/ui/digit_entry.c), which shows the same three states - a box still to
 * come, the box being set, and a box already set.
 *
 * The gain over the keypad is that the two buttons mean one thing here. On the
 * keypad they walk a cursor across twelve keys and the reader has to find the
 * digit; here up and down *are* the digit, and the only other move is
 * committing it. */
#define DIGIT_BOX_WIDTH 34
#define DIGIT_BOX_HEIGHT 32
#define DIGIT_BOX_GAP 6
#define DIGIT_ARROW_WIDTH 11
/* Six rows, not seven: the half-width steps in whole pixels, so an 11-wide
 * triangle has exactly six distinct rows (1, 3, 5, 7, 9, 11). A seventh row
 * has no width left of its own and repeats one, which reads as a flat spot in
 * the edge rather than a straight diagonal. */
#define DIGIT_ARROW_HEIGHT 6

/* An arrow, the boxes, an arrow: one stack, sitting at a different height on
 * each screen that draws it because what is under it differs - the value box
 * shares its screen with the entropy quality bar, the digit field has only the
 * footer. Derived from the stack's own parts, so changing an arrow's height
 * moves the stack rather than silently unbalancing it. */
#define DIGIT_FIELD_GAP 4
#define DIGIT_FIELD_HEIGHT (2 * (DIGIT_ARROW_HEIGHT + DIGIT_FIELD_GAP) + DIGIT_BOX_HEIGHT)
#define DIGIT_FIELD_TOP(bottom) (SCREEN_TITLE_BOTTOM + ((bottom) -SCREEN_TITLE_BOTTOM - DIGIT_FIELD_HEIGHT) / 2)
#define DIGIT_FIELD_BOX_Y(top) ((top) + DIGIT_ARROW_HEIGHT + DIGIT_FIELD_GAP)
#define DIGIT_FIELD_DOWN_Y(top) (DIGIT_FIELD_BOX_Y(top) + DIGIT_BOX_HEIGHT + DIGIT_FIELD_GAP)

/* Held rather than eyeballed. The widest field is one box per digit a word
 * number can have, and it is centred, so a box or gap grown by a few pixels
 * pushes the outer boxes off the edge - where fill_rect clips them silently and
 * the field simply looks wrong on the device rather than failing anywhere. The
 * vertical pair does the same for the arrows, which sit outside the boxes and
 * must clear both the title above and the footer below. */
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


/* One wider box for a value scrolled whole rather than digit by digit: a die
 * face is 1-6 and a D20 is 1-20, so splitting it into per-digit boxes would
 * ask for two presses where the value itself is the thing being chosen.
 * `back` draws the way out on the same ring, as the digit field does. */
#define VALUE_BOX_WIDTH 60

void seedtool_render_value_box(
    const char* title, const char* text, const bool back, const char* footer, const seedtool_progress_t* progress)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, SCREEN_TITLE_Y);
    /* Centred above the quality bar, not the footer: the bar is what shares
     * this screen, whether or not `progress` draws one this time - the box
     * should not jump between runs that grade their entropy and runs that do
     * not. */
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
            /* The way out of the field rides the same ring as the digits, so
             * the box shows the backspace glyph the keypad already uses for
             * it rather than a letter the reader has to decode. */
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
    /* The quality bar sits at DICE_BAR_Y=90..103 and the confirm bar starts at
     * 118, so the two clear each other without either moving: what the chrome
     * takes here is the footer's row, which is exactly what it replaces. */
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
            /* Letters and digits take the larger face; the keys still spelled
             * with a word keep the small one, because "OK" at 16px is 24px wide
             * in a 24px cell and would bleed into the key beside it. */
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

/* Scale/extent/position math shared by every screen that draws a square,
 * height-bound grid of `modules` cells with a title column to its right:
 * draw_qr and seedtool_render_qr_bytes_map both pass the QR's own size plus
 * its 2-module quiet zone, draw_qr_region passes a zoomed region's own
 * (quiet-zone-free) size - same shape at a different cell count either way,
 * so one computation serves all three instead of each re-deriving it. */
typedef struct {
    int scale;
    int extent;
    int top;
    int title_x;
    int title_width;
} qr_geometry_t;

static qr_geometry_t qr_geometry(const int modules)
{
    qr_geometry_t g;
    g.scale = SEEDTOOL_DISPLAY_HEIGHT / modules;
    g.extent = modules * g.scale;
    g.top = (SEEDTOOL_DISPLAY_HEIGHT - g.extent) / 2;
    g.title_x = QR_LEFT + g.extent + 5;
    g.title_width = SEEDTOOL_DISPLAY_WIDTH - g.title_x - 3;
    return g;
}

/* Shared by seedtool_render_qr and seedtool_render_qr_bytes: everything after
 * `qr`'s modules are populated, whichever encoder filled them. `modules` is
 * only needed back to zero it once drawn — it held whatever the mnemonic or
 * key material was encoded as. */
static bool draw_qr(QRCode* qr, uint8_t* modules, const char* title)
{
    /* The code sits left with its quiet zone intact and the title goes in the
     * margin beside it: on a screen that steps through several codes, one that
     * does not say what it holds is one that gets scanned into the wrong place.
     * The scale is the largest integer that still fits the display's height —
     * the binding dimension, since every version here is square and narrower
     * than the display is wide — so a smaller QR (Compact SeedQR's version 1
     * or 2) fills as much of the screen as it can rather than sitting at the
     * scale version 6 happens to need. */
    const qr_geometry_t g = qr_geometry(qr->size + 2);
    const int scale = g.scale;
    const int extent = g.extent;
    const int top = g.top;
    const int title_x = g.title_x;
    const int title_width = g.title_width;
    seedtool_render_clear();
    fill_rect(QR_LEFT, top, extent, extent, COLOR_WHITE);
    for (uint8_t y = 0; y < qr->size; ++y) {
        for (uint8_t x = 0; x < qr->size; ++x) {
            if (qrcode_getModule(qr, x, y)) {
                fill_rect(QR_LEFT + (x + 1) * scale, top + (y + 1) * scale, scale, scale, COLOR_BLACK);
            }
        }
    }
    draw_centered_box(tft_DefaultFont, title, title_x, title_width, QR_TITLE_Y);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
    return true;
}

bool seedtool_render_qr(const char* title, const char* text)
{
    /* Drawn at the smallest version that actually holds `text` -- like
     * seedtool_render_qr_bytes already does for Compact SeedQR -- rather than
     * always QR_VERSION, so a short value (an address, a single BBQr part)
     * fills the display with far coarser, easier-to-scan modules instead of
     * sitting at the same fine grid a full account key needs. */
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
    return draw_qr(&qr, modules, title);
}

size_t seedtool_render_qr_alphanumeric_capacity(const uint8_t max_version)
{
    size_t chars = 0;
    while (qrcode_versionForAlphanumeric(ECC_LOW, (uint16_t)(chars + 1), max_version)) {
        ++chars;
    }
    return chars;
}

bool seedtool_render_qr_bytes(const char* title, const uint8_t* data, const size_t len)
{
    if (len > UINT16_MAX) {
        return false;
    }
    /* Unlike the xpub/address QR above, a byte-mode payload (Compact SeedQR's
     * raw entropy) is drawn at the smallest version that holds it rather than
     * always QR_VERSION: that is the entire point of "compact" in the
     * SeedSigner/Krux convention this follows, and a fixed large version would
     * needlessly pad it with meaningless filler modules. `modules` stays sized
     * for QR_VERSION since qrcode_getBufferSize grows monotonically with
     * version, so it is a safe upper bound for the smaller version drawn here. */
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    if (!version) {
        return false;
    }
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    /* qrcode_initBytes only reads through `data` into its own codeword
     * buffer; this cast does not let it write through it. */
    if (qrcode_initBytes(&qr, modules, version, ECC_LOW, (uint8_t*)data, (uint16_t)len) != 0) {
        /* `data` here is raw seed entropy; a failed encode may already have
         * written some of it into `modules` before the encoder gave up. */
        memset(modules, 0, sizeof(modules));
        return false;
    }
    return draw_qr(&qr, modules, title);
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

/* The region map: the exact same full-size code seedtool_render_qr_bytes
 * draws (same QR_LEFT/scale/extent/title placement, so it looks like the
 * same screen, not a different one) with a dim line at every region boundary
 * and each region's own "A1" etc label stamped inside its own block - the
 * same qr_region_size/columns split, and the same dim-line-over-modules
 * convention draw_qr_region already uses on its own zoomed tile, just spaced
 * a region apart instead of a module apart. So the legend can never fall out
 * of step with what a zoomed tile actually shows, and the code is never
 * shrunk to make room for it. */
bool seedtool_render_qr_bytes_map(const char* title, const uint8_t* data, const size_t len)
{
    if (len > UINT16_MAX) {
        return false;
    }
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    if (!version) {
        return false;
    }
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (qrcode_initBytes(&qr, modules, version, ECC_LOW, (uint8_t*)data, (uint16_t)len) != 0) {
        memset(modules, 0, sizeof(modules));
        return false;
    }
    const size_t region_size = qr_region_size(qr.size);
    const size_t columns = ((size_t)qr.size + region_size - 1) / region_size;
    const qr_geometry_t g = qr_geometry(qr.size + 2);
    const int scale = g.scale;
    const int extent = g.extent;
    const int top = g.top;
    const int title_x = g.title_x;
    const int title_width = g.title_width;
    /* The code's own top-left corner, one quiet-zone module in from the white
     * fill's corner: where the boundary grid and the region labels are drawn
     * relative to, since the quiet zone itself has no region to divide. */
    const int code_left = QR_LEFT + scale;
    const int code_top = top + scale;
    const int code_extent = (int)qr.size * scale;
    const int block = (int)region_size * scale;
    seedtool_render_clear();
    fill_rect(QR_LEFT, top, extent, extent, COLOR_WHITE);
    for (uint8_t y = 0; y < qr.size; ++y) {
        for (uint8_t x = 0; x < qr.size; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                fill_rect(QR_LEFT + (x + 1) * scale, top + (y + 1) * scale, scale, scale, COLOR_BLACK);
            }
        }
    }
    for (size_t i = 0; i <= columns; ++i) {
        const int pos = (int)i * block;
        fill_rect(code_left, code_top + pos, code_extent, 1, COLOR_DIM);
        fill_rect(code_left + pos, code_top, 1, code_extent, COLOR_DIM);
    }
    /* A dim line over a QR module has one of two known backgrounds, black or
     * white, so it always reads against at least one of them. A label is
     * text, needed against both at once, and COLOR_DIM is too close to black
     * to read on a black module - so each one gets a small solid plate under
     * it first, sized to the block with a 1px gap from its boundary lines,
     * and black-on-white text on top of that instead of straight onto
     * whatever the code drew there. */
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
    draw_centered_box(tft_DefaultFont, title, title_x, title_width, QR_TITLE_Y);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
    return true;
}

/* Same layout draw_qr uses (a square bound by the display's height, with a
 * label in the margin to its right), just at the block's own scale instead of
 * the whole code's: with only 5-7 cells to fill instead of 21-25, each cell
 * comes out much larger, which is the entire point of "zoomed". No quiet zone
 * is drawn around it, unlike draw_qr's full code: a block is meant to be
 * hand-copied, never scanned on its own. */
static void draw_qr_region(QRCode* qr, uint8_t* modules, const char* title, const size_t region_size,
    const size_t columns, const size_t region_index)
{
    const size_t row = region_index / columns;
    const size_t column = region_index % columns;
    const qr_geometry_t g = qr_geometry((int)region_size);
    const int scale = g.scale;
    const int extent = g.extent;
    const int top = g.top;
    const int title_x = g.title_x;
    const int title_width = g.title_width;
    seedtool_render_clear();
    for (size_t y = 0; y < region_size; ++y) {
        for (size_t x = 0; x < region_size; ++x) {
            const size_t qx = column * region_size + x;
            const size_t qy = row * region_size + y;
            const bool set = qx < qr->size && qy < qr->size && qrcode_getModule(qr, (uint8_t)qx, (uint8_t)qy);
            fill_rect(QR_LEFT + (int)x * scale, top + (int)y * scale, scale, scale, set ? COLOR_BLACK : COLOR_WHITE);
        }
    }
    for (size_t i = 0; i <= region_size; ++i) {
        fill_rect(QR_LEFT, top + (int)i * scale, extent, 1, COLOR_DIM);
        fill_rect(QR_LEFT + (int)i * scale, top, 1, extent, COLOR_DIM);
    }
    const int label_y = draw_centered_box(tft_DefaultFont, title, title_x, title_width, QR_TITLE_Y);
    char label[24];
    (void)snprintf(label, sizeof(label), "Region %c%u", (char)('A' + row), (unsigned)(column + 1));
    draw_centered_box(tft_DefaultFont, label, title_x, title_width, label_y + 4);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
}

bool seedtool_render_qr_bytes_region(const char* title, const uint8_t* data, const size_t len, const size_t region_index)
{
    if (len > UINT16_MAX) {
        return false;
    }
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    if (!version) {
        return false;
    }
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (qrcode_initBytes(&qr, modules, version, ECC_LOW, (uint8_t*)data, (uint16_t)len) != 0) {
        /* `data` here is raw seed entropy (Compact SeedQR's only caller); a
         * failed encode may already have written some of it into `modules`
         * before the encoder gave up. */
        memset(modules, 0, sizeof(modules));
        return false;
    }
    const size_t region_size = qr_region_size(qr.size);
    const size_t columns = ((size_t)qr.size + region_size - 1) / region_size;
    if (region_index >= columns * columns) {
        memset(modules, 0, sizeof(modules));
        return false;
    }
    draw_qr_region(&qr, modules, title, region_size, columns, region_index);
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
