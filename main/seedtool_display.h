#ifndef SEEDTOOL_DISPLAY_H_
#define SEEDTOOL_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>

#include "seedtool_render.h"

#define SEEDTOOL_DISPLAY_BRIGHTNESS_MIN 1
#define SEEDTOOL_DISPLAY_BRIGHTNESS_MAX 5
/* What the backlight opens at, and what seedtool_app.c's own backlight_level
 * must start at too - the two have to agree, or the Settings screen would
 * show a level the hardware isn't actually at. */
#define SEEDTOOL_DISPLAY_BRIGHTNESS_DEFAULT 3

/* Shared by every seedtool_display_set_brightness backend, so each platform
 * does not carry its own copy of the same bounds check. */
static inline unsigned seedtool_display_clamp_brightness(const unsigned level)
{
    return level < SEEDTOOL_DISPLAY_BRIGHTNESS_MIN ? SEEDTOOL_DISPLAY_BRIGHTNESS_MIN
        : level > SEEDTOOL_DISPLAY_BRIGHTNESS_MAX ? SEEDTOOL_DISPLAY_BRIGHTNESS_MAX : level;
}

void seedtool_display_init(void);
void seedtool_display_set_orientation(bool flipped);
void seedtool_display_set_brightness(unsigned level);
void seedtool_display_screen(const char* title, const char* line1, const char* line2, const char* footer);
void seedtool_display_screen3(
    const char* title, const char* line1, const char* line2, const char* line3, const char* footer);
void seedtool_display_screen4(const char* title, const char* line1, const char* line2, const char* line3,
    const char* line4, const char* footer);
void seedtool_display_dice_screen(const char* title, const char* line1, const char* line2, const char* footer,
    const seedtool_progress_t* progress);
void seedtool_display_splash(void);
void seedtool_display_list(const char* title, const char* const* items, size_t count, size_t selected, size_t top,
    const char* footer);
void seedtool_display_keyboard(
    const char* title, const char* text, const char* layout, const bool* enabled, size_t selected);
bool seedtool_display_qr(const char* title, const char* text);
bool seedtool_display_qr_bytes(const char* title, const uint8_t* data, size_t len);
bool seedtool_display_qr_bytes_region(const char* title, const uint8_t* data, size_t len, size_t region_index);
bool seedtool_display_qr_bytes_map(const char* title, const uint8_t* data, size_t len);
void seedtool_display_stackbit_screen(const char* title, unsigned word_number, const char* word, const char* footer);
void seedtool_display_stackbit_physical_screen(
    const char* title, unsigned word_number, const char* word, const char* footer);

#endif
