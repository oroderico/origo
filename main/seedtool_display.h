#ifndef SEEDTOOL_DISPLAY_H_
#define SEEDTOOL_DISPLAY_H_

#include <stdbool.h>

#define SEEDTOOL_DISPLAY_WIDTH 240
#define SEEDTOOL_DISPLAY_HEIGHT 135
#define SEEDTOOL_BUTTON_LEFT_GPIO 35
#define SEEDTOOL_BUTTON_RIGHT_GPIO 0

void seedtool_display_init(void);
void seedtool_display_screen(const char* title, const char* line1, const char* line2, const char* footer);
bool seedtool_display_qr(const char* text);

#endif
