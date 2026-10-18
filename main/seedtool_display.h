#ifndef SEEDTOOL_DISPLAY_H_
#define SEEDTOOL_DISPLAY_H_

#include <stdbool.h>

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
/* Sends the frame the last seedtool_render_* call drew to the panel. */
void seedtool_display_flush(void);

#endif
