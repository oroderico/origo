/* The input screens: keyboards, digit and value boxes, the bit tally, levels
 * and Stackbit. Drawn with the primitives in render_internal.h. */
#include "render_internal.h"

#include <stdio.h>
#include <string.h>

/* The band under an entry field, in the gap seedtool_render_screen leaves
 * between its second line (ends y=81) and the footer (starts y=111): the
 * brightness level's segments and the bit tally fill it, the keyboard's word
 * bar spans its width, and the value box centres above it. */
#define BAND_MARGIN 20
#define BAND_X BAND_MARGIN
#define BAND_WIDTH (SEEDTOOL_DISPLAY_WIDTH - 2 * BAND_MARGIN)
#define BAND_Y 90
#define BAND_HEIGHT 14

#define KEYBOARD_COLUMNS 10
#define KEY_WIDTH (SEEDTOOL_DISPLAY_WIDTH / KEYBOARD_COLUMNS)
#define KEY_HEIGHT 27
#define KEYBOARD_TOP (SEEDTOOL_DISPLAY_HEIGHT - 3 * KEY_HEIGHT)

/* In the 6px gap between the typed text (ends y=48) and the keyboard (starts
 * y=54), with the entropy bar's margins. */
#define WORD_BAR_Y (KEYBOARD_TOP - 4)
#define WORD_BAR_HEIGHT 3

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
_Static_assert(DIGIT_FIELD_TOP(BAND_Y) >= SCREEN_TITLE_BOTTOM, "the value box runs into the title");
_Static_assert(DIGIT_FIELD_DOWN_Y(DIGIT_FIELD_TOP(BAND_Y)) + DIGIT_ARROW_HEIGHT <= BAND_Y,
    "the value box runs into the band below it");

/* One wider box for a value scrolled whole, a die face or a D20. `back` puts
 * the way out on the ring, as the digit field does. */
#define VALUE_BOX_WIDTH 60

/* The tally's count to the right of its bar, in a slot sized for the widest
 * count it will show, so the bar keeps one length while the count grows. */
#define TALLY_GAP 8

void draw_tally(const seedtool_tally_t* tally)
{
    if (!tally || !tally->total) {
        return;
    }
    const unsigned done = tally->done > tally->total ? tally->total : tally->done;
    char count[32], widest[32];
    (void)snprintf(count, sizeof(count), "%u/%u bits", done, tally->total);
    (void)snprintf(widest, sizeof(widest), "%u/%u bits", tally->total, tally->total);
    const int slot = text_width(tft_DefaultFont, widest, strlen(widest));
    const int bar_width = BAND_WIDTH - TALLY_GAP - slot;
    const int filled = (int)((unsigned)(bar_width - 4) * done / tally->total);
    if (filled > 0) {
        fill_rect(BAND_X + 2, BAND_Y + 2, filled, BAND_HEIGHT - 4, COLOR_HIGHLIGHT);
    }
    draw_border(BAND_X, BAND_Y, bar_width, BAND_HEIGHT, COLOR_DIM);
    const int text_x = BAND_X + BAND_WIDTH - text_width(tft_DefaultFont, count, strlen(count));
    draw_line_at(tft_DefaultFont, count, strlen(count), text_x, BAND_Y + (BAND_HEIGHT - tft_DefaultFont[1]) / 2,
        COLOR_SOFT);
}

void seedtool_render_value_box(
    const char* title, const char* text, const bool back, const char* footer, const seedtool_tally_t* tally)
{
    seedtool_render_clear();
    draw_centered(FONT_TITLE, title, SCREEN_TITLE_Y);
    const int top = DIGIT_FIELD_TOP(BAND_Y);
    const int box_y = DIGIT_FIELD_BOX_Y(top);
    const int x = (SEEDTOOL_DISPLAY_WIDTH - VALUE_BOX_WIDTH) / 2;
    fill_rect(x, box_y, VALUE_BOX_WIDTH, DIGIT_BOX_HEIGHT, COLOR_HIGHLIGHT);
    draw_border(x, box_y, VALUE_BOX_WIDTH, DIGIT_BOX_HEIGHT, COLOR_HIGHLIGHT);
    if (back) {
        draw_backspace(x + (VALUE_BOX_WIDTH - BACKSPACE_WIDTH) / 2,
            box_y + (DIGIT_BOX_HEIGHT - BACKSPACE_HEIGHT) / 2, COLOR_BLACK, COLOR_HIGHLIGHT);
    } else {
        draw_centered_in(FONT_BODY, text, x, VALUE_BOX_WIDTH,
            box_y + (DIGIT_BOX_HEIGHT - FONT_BODY[1]) / 2, COLOR_BLACK);
    }
    const int arrow_x = x + (VALUE_BOX_WIDTH - DIGIT_ARROW_WIDTH) / 2;
    draw_triangle(arrow_x, top, DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_UP, COLOR_WHITE);
    draw_triangle(arrow_x, DIGIT_FIELD_DOWN_Y(top), DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_DOWN, COLOR_WHITE);
    draw_centered(tft_DefaultFont, footer, SCREEN_FOOTER_Y);
    draw_tally(tally);
}

#define LEVEL_SEGMENT_GAP 4

void seedtool_render_level(const char* title, const unsigned level, const unsigned max, const char* footer)
{
    seedtool_render_clear();
    draw_centered(FONT_TITLE, title, SCREEN_TITLE_Y);
    const int top = DIGIT_FIELD_TOP(BAND_Y);
    const int box_y = DIGIT_FIELD_BOX_Y(top);
    const int x = (SEEDTOOL_DISPLAY_WIDTH - VALUE_BOX_WIDTH) / 2;
    char text[24];
    (void)snprintf(text, sizeof(text), "%u/%u", level, max);
    fill_rect(x, box_y, VALUE_BOX_WIDTH, DIGIT_BOX_HEIGHT, COLOR_HIGHLIGHT);
    draw_centered_in(FONT_BODY, text, x, VALUE_BOX_WIDTH, box_y + (DIGIT_BOX_HEIGHT - FONT_BODY[1]) / 2,
        COLOR_BLACK);
    /* At either end the arrow that would go nowhere is left out, which is
     * the screen's whole answer to a press that changes nothing. */
    const int arrow_x = x + (VALUE_BOX_WIDTH - DIGIT_ARROW_WIDTH) / 2;
    if (level < max) {
        draw_triangle(arrow_x, top, DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_UP, COLOR_WHITE);
    }
    if (level > 1) {
        draw_triangle(arrow_x, DIGIT_FIELD_DOWN_Y(top), DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_DOWN, COLOR_WHITE);
    }
    if (max) {
        const int segment = (BAND_WIDTH - (int)(max - 1) * LEVEL_SEGMENT_GAP) / (int)max;
        for (unsigned i = 0; i < max; ++i) {
            const int sx = BAND_X + (int)i * (segment + LEVEL_SEGMENT_GAP);
            if (i < level) {
                fill_rect(sx, BAND_Y, segment, BAND_HEIGHT, COLOR_HIGHLIGHT);
            } else {
                draw_border(sx, BAND_Y, segment, BAND_HEIGHT, COLOR_DIM);
            }
        }
    }
    draw_centered(tft_DefaultFont, footer, SCREEN_FOOTER_Y);
}

void seedtool_render_digits(
    const char* title, const char* digits, const size_t count, const size_t active, const char* footer)
{
    seedtool_render_clear();
    draw_centered(FONT_TITLE, title, SCREEN_TITLE_Y);

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
        const int glyph_y = box_y + (DIGIT_BOX_HEIGHT - FONT_BODY[1]) / 2;
        if (shown == SEEDTOOL_KEY_ACCEPT) {
            draw_centered_in(FONT_BODY, "OK", x, DIGIT_BOX_WIDTH, glyph_y, selected ? COLOR_BLACK : COLOR_WHITE);
        } else if (shown == SEEDTOOL_KEY_BACKSPACE) {
            /* The way out rides the ring as the backspace glyph. */
            draw_backspace(x + (DIGIT_BOX_WIDTH - BACKSPACE_WIDTH) / 2, box_y + (DIGIT_BOX_HEIGHT - BACKSPACE_HEIGHT) / 2,
                COLOR_BLACK, COLOR_HIGHLIGHT);
        } else if (shown && shown != ' ') {
            const char text[2] = { shown, '\0' };
            draw_centered_in(FONT_BODY, text, x, DIGIT_BOX_WIDTH, glyph_y, selected ? COLOR_BLACK : COLOR_WHITE);
        }

        if (selected) {
            const int arrow_x = x + (DIGIT_BOX_WIDTH - DIGIT_ARROW_WIDTH) / 2;
            draw_triangle(arrow_x, top, DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_UP, COLOR_WHITE);
            draw_triangle(
                arrow_x, DIGIT_FIELD_DOWN_Y(top), DIGIT_ARROW_WIDTH, DIGIT_ARROW_HEIGHT, TRIANGLE_DOWN, COLOR_WHITE);
        }
    }

    draw_centered(tft_DefaultFont, footer, SCREEN_FOOTER_Y);
}

void seedtool_render_keyboard(const char* title, const char* text, const char* layout, const bool* enabled,
    const size_t selected, const size_t position, const size_t total)
{
    seedtool_render_clear();
    draw_centered(FONT_TITLE, title, 2);
    draw_typed(FONT_BODY, text, 32);
    if (total && position) {
        const size_t done = position > total ? total : position - 1;
        const int filled = (int)(BAND_WIDTH * done / total);
        fill_rect(BAND_X, WORD_BAR_Y, BAND_WIDTH, WORD_BAR_HEIGHT, COLOR_DIM);
        if (filled) {
            fill_rect(BAND_X, WORD_BAR_Y, filled, WORD_BAR_HEIGHT, COLOR_HIGHLIGHT);
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
            const unsigned char* const face = worded ? tft_DefaultFont : FONT_BODY;
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
    draw_centered_in(FONT_BODY, digits, STACKBIT_PANEL_X, STACKBIT_PANEL_WIDTH, STACKBIT_NUMBER_Y, COLOR_WHITE);
    draw_centered_in(tft_DefaultFont, word, STACKBIT_PANEL_X, STACKBIT_PANEL_WIDTH, STACKBIT_WORD_Y, COLOR_SOFT);
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

    draw_centered(FONT_BODY, digits, STACKBIT_PHYS_NUMBER_Y);
    draw_centered_in(tft_DefaultFont, word, 0, SEEDTOOL_DISPLAY_WIDTH, STACKBIT_PHYS_WORD_Y, COLOR_SOFT);
    draw_centered(tft_DefaultFont, footer, STACKBIT_PHYS_FOOTER_Y);
}
