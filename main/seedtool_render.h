#ifndef SEEDTOOL_RENDER_H_
#define SEEDTOOL_RENDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_DISPLAY_WIDTH 240
#define SEEDTOOL_DISPLAY_HEIGHT 135

/* Keyboard control keys. They occupy one cell of the layout like any other key
 * but are drawn as a short word instead of a glyph. */
#define SEEDTOOL_KEY_BACKSPACE '\b'
#define SEEDTOOL_KEY_PAGE '\t'
#define SEEDTOOL_KEY_ACCEPT '\r'

void seedtool_render_clear(void);
void seedtool_render_screen(const char* title, const char* line1, const char* line2, const char* footer);

/* How many leading characters of `text` fit on one body line, up to `limit`.
 * The body font is proportional, so callers must split by this rather than by a
 * fixed character count: an address silently wrapped onto the next row would be
 * transcribed wrongly. */
size_t seedtool_render_fit(const char* text, size_t limit);

/* `layout` is the key characters row by row, rows separated by '\n' and at most
 * ten keys per row. `enabled` is indexed by key position across the whole
 * layout, ignoring the separators; disabled keys are drawn dimmed and are meant
 * to be unreachable. `selected` is a key position and is drawn highlighted. */
void seedtool_render_keyboard(
    const char* title, const char* text, const char* layout, const bool* enabled, size_t selected);

bool seedtool_render_qr(const char* text);
const uint16_t* seedtool_render_pixels(void);

#endif
