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
void seedtool_display_dice_screen(const char* title, const char* line1, const char* line2, const char* footer,
    const seedtool_progress_t* progress);
void seedtool_display_splash(void);
/* One wrapper per nav screen shape, each just its renderer plus a flush. A
 * new shape needs one here and one in each backend; everything about the
 * chrome itself is already carried by seedtool_nav_t. */
void seedtool_display_nav_text(
    const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2, const char* line3);
void seedtool_display_nav_grouped(const seedtool_nav_t* nav, const char* title, const char* const* lines,
    const size_t* first_group, size_t count);
void seedtool_display_nav_rows(const seedtool_nav_t* nav, const char* title, const char* const* rows, size_t count);
void seedtool_display_nav_list(
    const seedtool_nav_t* nav, const char* title, const char* const* items, size_t count, size_t top);
void seedtool_display_nav_dice(const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2,
    const seedtool_progress_t* progress);
void seedtool_display_digits(const char* title, const char* digits, size_t count, size_t active, const char* footer,
    const seedtool_progress_t* progress);
void seedtool_display_value_box(
    const char* title, const char* text, bool back, const char* footer, const seedtool_progress_t* progress);
void seedtool_display_keyboard(const char* title, const char* text, const char* layout, const bool* enabled,
    size_t selected, size_t position, size_t total);
bool seedtool_display_qr(const char* title, const char* text);
bool seedtool_display_qr_bytes(const char* title, const uint8_t* data, size_t len);
bool seedtool_display_qr_bytes_region(const char* title, const uint8_t* data, size_t len, size_t region_index);
bool seedtool_display_qr_bytes_map(const char* title, const uint8_t* data, size_t len);
void seedtool_display_stackbit_screen(
    const seedtool_nav_t* nav, const char* title, unsigned word_number, const char* word, const char* footer);
void seedtool_display_stackbit_physical_screen(
    const seedtool_nav_t* nav, const char* title, unsigned word_number, const char* word, const char* footer);

#endif
