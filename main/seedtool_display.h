#ifndef SEEDTOOL_DISPLAY_H_
#define SEEDTOOL_DISPLAY_H_

#include <stdbool.h>

void seedtool_display_init(void);
void seedtool_display_screen(const char* title, const char* line1, const char* line2, const char* footer);
bool seedtool_display_qr(const char* text);

#endif
