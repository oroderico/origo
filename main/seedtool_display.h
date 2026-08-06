#ifndef SEEDTOOL_DISPLAY_H_
#define SEEDTOOL_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>

#include "seedtool_render.h"

void seedtool_display_init(void);
void seedtool_display_screen(const char* title, const char* line1, const char* line2, const char* footer);
void seedtool_display_screen3(
    const char* title, const char* line1, const char* line2, const char* line3, const char* footer);
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
void seedtool_display_stackbit_screen(const char* title, unsigned word_number, const char* word, const char* footer);
void seedtool_display_stackbit_physical_screen(
    const char* title, unsigned word_number, const char* word, const char* footer);

#endif
