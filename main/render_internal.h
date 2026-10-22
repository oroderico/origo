/* What the renderer's three files share. seedtool_render.c owns the
 * framebuffer and draws text, the nav chrome, lists and the plain screens;
 * render_entry.c the input screens - keyboards, digit and value boxes, dice,
 * Stackbit; render_qr.c every QR screen. Nothing outside the renderer includes
 * this: the rest of the firmware sees seedtool_render.h only. */
#ifndef RENDER_INTERNAL_H_
#define RENDER_INTERNAL_H_

#include "seedtool_render.h"

#include <stddef.h>
#include <stdint.h>

#define COLOR_BLACK UINT16_C(0x0000)
#define COLOR_WHITE UINT16_C(0xffff)
#define COLOR_DIM UINT16_C(0x39e7)
/* Secondary text that is still read: lighter than COLOR_DIM, which marks what
 * is inactive or structural. */
#define COLOR_SOFT UINT16_C(0xa514)
#define COLOR_HIGHLIGHT UINT16_C(0xfd20)
#define COLOR_WARN UINT16_C(0xf800)
#define COLOR_GO UINT16_C(0x07e0)
/* The master fingerprint where it names the wallet: a light shade of the logo's
 * leaf green, clear of COLOR_HIGHLIGHT, which means selected. */
#define COLOR_FINGERPRINT UINT16_C(0xbe4b)

/* The band between a plain screen's title and footer; the digit field centres
 * in it. */
#define SCREEN_TITLE_Y 2
#define SCREEN_TITLE_BOTTOM (SCREEN_TITLE_Y + 16) /* the 16px face's own height */
#define SCREEN_FOOTER_Y 111

/* Nav chrome. The back arrow sits inside the title bar, and the title is
 * centred between margins of NAV_BACK_WIDTH so the arrow does not push it off
 * centre. */
#define NAV_BACK_X 2
#define NAV_BACK_Y 1
#define NAV_BACK_WIDTH 20
#define NAV_BACK_HEIGHT 19
#define NAV_ARROW_WIDTH 7
#define NAV_ARROW_HEIGHT 11

extern const unsigned char tft_DefaultFont[];
extern const unsigned char tft_UbuntuRegular16[];
extern const unsigned char tft_UbuntuMedium16[];

/* Screen titles in the regular weight and what the screen holds - menu rows,
 * words, numbers, addresses, keys - in the medium one, as Jade does: the
 * weight marks what is read, not what labels it. */
#define FONT_TITLE tft_UbuntuRegular16
#define FONT_BODY tft_UbuntuMedium16

/* An address has no word boundaries to keep one's place by, so it is drawn in
 * groups of four with a gap and an alternating ink: two signals for where a
 * group ends. Grouping is only drawn; what is compared, hashed and encoded is
 * the unbroken string. */
#define GROUP_LEN SEEDTOOL_GROUP_LEN
#define GROUP_GAP 5

/* Which way a triangle's apex points. */
typedef enum { TRIANGLE_UP, TRIANGLE_DOWN, TRIANGLE_LEFT, TRIANGLE_RIGHT } triangle_dir_t;

/* seedtool_render.c: the primitives the other two draw with. Each is
 * described where it is defined. */
void fill_rect(int x, int y, int width, int height, const uint16_t color);
int glyph_advance(const uint8_t* font, const unsigned char character);
int text_width(const uint8_t* font, const char* text, const size_t length);
void draw_glyph(const uint8_t* font, const unsigned char character, const int x, const int y, const uint16_t color);
void draw_line_at(const uint8_t* font, const char* text, const size_t length, int x, const int y, const uint16_t color);
void draw_centered_in(
    const uint8_t* font, const char* text, const int x, const int width, const int y, const uint16_t color);
int layout_centered_box(const uint8_t* font, const char* text, const int x, const int width, int y, const bool draw);
int draw_centered_box(const uint8_t* font, const char* text, const int x, const int width, const int y);
int text_ink_bottom(const uint8_t* font, const char* text, const size_t length);
void draw_centered(const uint8_t* font, const char* text, const int y);
void draw_typed(const uint8_t* font, const char* text, const int y);
uint16_t group_ink(const size_t index);
void draw_triangle(
    const int x, const int y, const int width, const int height, const triangle_dir_t dir, const uint16_t color);
void draw_border(const int x, const int y, const int width, const int height, const uint16_t color);
void nav_begin(const seedtool_nav_t* nav, const char* title);

/* render_entry.c: the tally in the band under an entry field; NULL draws
 * nothing. */
void draw_tally(const seedtool_tally_t* tally);

#endif
