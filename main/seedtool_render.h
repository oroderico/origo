#ifndef SEEDTOOL_RENDER_H_
#define SEEDTOOL_RENDER_H_

#include <stdbool.h>
#include <stdint.h>

#define SEEDTOOL_DISPLAY_WIDTH 240
#define SEEDTOOL_DISPLAY_HEIGHT 135

void seedtool_render_clear(void);
void seedtool_render_screen(const char* title, const char* line1, const char* line2, const char* footer);
bool seedtool_render_qr(const char* text);
const uint16_t* seedtool_render_pixels(void);

#endif
