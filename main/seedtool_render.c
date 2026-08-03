#include "seedtool_render.h"

#include "qrcode.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define COLOR_BLACK UINT16_C(0x0000)
#define COLOR_WHITE UINT16_C(0xffff)
#define COLOR_DIM UINT16_C(0x39e7)
#define COLOR_HIGHLIGHT UINT16_C(0xfd20)

#define KEYBOARD_COLUMNS 10
#define KEY_WIDTH (SEEDTOOL_DISPLAY_WIDTH / KEYBOARD_COLUMNS)
#define KEY_HEIGHT 27
#define KEYBOARD_TOP (SEEDTOOL_DISPLAY_HEIGHT - 3 * KEY_HEIGHT)

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

static void draw_centered_line(const uint8_t* font, const char* text, const size_t length, const int y)
{
    int x = (SEEDTOOL_DISPLAY_WIDTH - text_width(font, text, length)) / 2;
    if (x < 0) {
        x = 0;
    }
    draw_line_at(font, text, length, x, y, COLOR_WHITE);
}

static void draw_centered(const uint8_t* font, const char* text, int y)
{
    if (!text) {
        return;
    }
    const int line_height = font[1];
    const char* cursor = text;
    while (*cursor && y + line_height <= SEEDTOOL_DISPLAY_HEIGHT) {
        const char* end = cursor;
        int width = 0;
        while (*end && *end != '\n') {
            const int advance = glyph_advance(font, (unsigned char)*end);
            if (end != cursor && width + advance > SEEDTOOL_DISPLAY_WIDTH) {
                break;
            }
            width += advance;
            ++end;
        }
        draw_centered_line(font, cursor, (size_t)(end - cursor), y);
        cursor = *end == '\n' ? end + 1 : end;
        y += line_height;
    }
}

size_t seedtool_render_fit(const char* text, const size_t limit)
{
    int width = 0;
    size_t count = 0;
    for (; count < limit && text[count]; ++count) {
        const int advance = glyph_advance(tft_DefaultFont, (unsigned char)text[count]);
        if (width + advance > SEEDTOOL_DISPLAY_WIDTH - 4) {
            break;
        }
        width += advance;
    }
    return count;
}

void seedtool_render_clear(void) { fill_rect(0, 0, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT, COLOR_BLACK); }

void seedtool_render_screen(const char* title, const char* line1, const char* line2, const char* footer)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, 5);
    draw_centered(tft_DefaultFont, line1, 39);
    draw_centered(tft_DefaultFont, line2, 65);
    draw_centered(tft_DefaultFont, footer, 111);
}

static const char* key_label(const char key, char* scratch)
{
    switch (key) {
    case SEEDTOOL_KEY_BACKSPACE:
        return "del";
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

static void draw_border(const int x, const int y, const int width, const int height, const uint16_t color)
{
    fill_rect(x, y, width, 1, color);
    fill_rect(x, y + height - 1, width, 1, color);
    fill_rect(x, y, 1, height, color);
    fill_rect(x + width - 1, y, 1, height, color);
}

void seedtool_render_keyboard(
    const char* title, const char* text, const char* layout, const bool* enabled, const size_t selected)
{
    seedtool_render_clear();
    draw_centered(tft_Ubuntu16, title, 2);
    draw_centered(tft_DefaultFont, text, 32);

    size_t key = 0;
    int y = KEYBOARD_TOP;
    for (const char* row = layout; *row;) {
        const char* end = row;
        while (*end && *end != '\n') {
            ++end;
        }
        const int count = (int)(end - row);
        const int left = (SEEDTOOL_DISPLAY_WIDTH - count * KEY_WIDTH) / 2;
        for (int column = 0; column < count; ++column, ++key) {
            const bool active = !enabled || enabled[key];
            const bool highlighted = key == selected;
            const int x = left + column * KEY_WIDTH;
            char scratch[2];
            const char* const label = key_label(row[column], scratch);
            if (highlighted) {
                fill_rect(x + 1, y + 1, KEY_WIDTH - 2, KEY_HEIGHT - 3, COLOR_HIGHLIGHT);
            } else {
                draw_border(x + 1, y + 1, KEY_WIDTH - 2, KEY_HEIGHT - 3, active ? COLOR_DIM : COLOR_BLACK);
            }
            const uint16_t ink = highlighted ? COLOR_BLACK : active ? COLOR_WHITE : COLOR_DIM;
            draw_centered_in(tft_DefaultFont, label, x, KEY_WIDTH, y + (KEY_HEIGHT - tft_DefaultFont[1]) / 2, ink);
        }
        y += KEY_HEIGHT;
        row = *end ? end + 1 : end;
    }
}

bool seedtool_render_qr(const char* text)
{
    uint8_t modules[qrcode_getBufferSize(5)];
    QRCode qr;
    if (qrcode_initText(&qr, modules, 5, ECC_LOW, text) != 0) {
        return false;
    }
    const int scale = 3;
    const int extent = (qr.size + 2) * scale;
    const int left = (SEEDTOOL_DISPLAY_WIDTH - extent) / 2;
    const int top = (SEEDTOOL_DISPLAY_HEIGHT - extent) / 2;
    seedtool_render_clear();
    fill_rect(left, top, extent, extent, COLOR_WHITE);
    for (uint8_t y = 0; y < qr.size; ++y) {
        for (uint8_t x = 0; x < qr.size; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                fill_rect(left + (x + 1) * scale, top + (y + 1) * scale, scale, scale, COLOR_BLACK);
            }
        }
    }
    memset(modules, 0, sizeof(modules));
    return true;
}

const uint16_t* seedtool_render_pixels(void) { return framebuffer; }
