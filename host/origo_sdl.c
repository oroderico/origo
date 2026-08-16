#include "seedtool_display.h"
#include "seedtool_platform.h"
#include "seedtool_render.h"

#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define WINDOW_SCALE 4

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* texture;

/* Mirror the hardware driver's session-only settings so the simulator gives
 * the same live feedback: a 180-degree flip is horizontal+vertical mirroring,
 * and brightness is approximated by scaling the texture's color channels. */
static SDL_RendererFlip orientation_flip = SDL_FLIP_NONE;
static uint8_t brightness_mod = 255;

static _Noreturn void sdl_fail(const char* operation)
{
    fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    exit(EXIT_FAILURE);
}

/* Redraws the last-uploaded frame with the current flip/color-mod state, with
 * no texture reupload - for the flip and brightness setters below, where the
 * pixel content hasn't changed, only how it's presented. */
static void redraw(void)
{
    SDL_SetTextureColorMod(texture, brightness_mod, brightness_mod, brightness_mod);
    SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopyEx(renderer, texture, NULL, NULL, 0.0, NULL, orientation_flip);
    SDL_RenderPresent(renderer);
}

static void present(void)
{
    if (SDL_UpdateTexture(texture, NULL, seedtool_render_pixels(), SEEDTOOL_DISPLAY_WIDTH * sizeof(uint16_t)) != 0) {
        sdl_fail("SDL_UpdateTexture");
    }
    redraw();
}

void seedtool_display_set_orientation(const bool flipped)
{
    orientation_flip = flipped ? (SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL) : SDL_FLIP_NONE;
    redraw();
}

void seedtool_display_set_brightness(const unsigned level)
{
    const unsigned clamped = seedtool_display_clamp_brightness(level);
    brightness_mod = (uint8_t)(clamped * 255 / SEEDTOOL_DISPLAY_BRIGHTNESS_MAX);
    redraw();
}

void seedtool_display_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        sdl_fail("SDL_Init");
    }
    window = SDL_CreateWindow("Origo simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SEEDTOOL_DISPLAY_WIDTH * WINDOW_SCALE, SEEDTOOL_DISPLAY_HEIGHT * WINDOW_SCALE, SDL_WINDOW_RESIZABLE);
    if (!window) {
        sdl_fail("SDL_CreateWindow");
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer || SDL_RenderSetLogicalSize(renderer, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT) != 0) {
        sdl_fail("SDL_CreateRenderer");
    }
    texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT);
    if (!texture) {
        sdl_fail("SDL_CreateTexture");
    }
    seedtool_render_clear();
    present();
}

void seedtool_display_screen(const char* title, const char* line1, const char* line2, const char* footer)
{
    seedtool_render_screen(title, line1, line2, footer);
    present();
}

void seedtool_display_dice_screen(const char* title, const char* line1, const char* line2, const char* footer,
    const seedtool_progress_t* progress)
{
    seedtool_render_dice_screen(title, line1, line2, footer, progress);
    present();
}

void seedtool_display_digits(const char* title, const char* digits, const size_t count, const size_t active,
    const char* footer, const seedtool_progress_t* progress)
{
    seedtool_render_digits(title, digits, count, active, footer, progress);
    present();
}

void seedtool_display_value_box(
    const char* title, const char* text, const bool back, const char* footer, const seedtool_progress_t* progress)
{
    seedtool_render_value_box(title, text, back, footer, progress);
    present();
}

void seedtool_display_splash(void)
{
    seedtool_render_splash();
    present();
}

void seedtool_display_nav_text(
    const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2, const char* line3)
{
    seedtool_render_nav_text(nav, title, line1, line2, line3);
    present();
}

void seedtool_display_nav_grouped(const seedtool_nav_t* nav, const char* title, const char* const* lines,
    const size_t* first_group, const size_t count)
{
    seedtool_render_nav_grouped(nav, title, lines, first_group, count);
    present();
}

void seedtool_display_nav_rows(const seedtool_nav_t* nav, const char* title, const char* const* rows, const size_t count)
{
    seedtool_render_nav_rows(nav, title, rows, count);
    present();
}

void seedtool_display_nav_list(
    const seedtool_nav_t* nav, const char* title, const char* const* items, const size_t count, const size_t top)
{
    seedtool_render_nav_list(nav, title, items, count, top);
    present();
}

void seedtool_display_nav_dice(const seedtool_nav_t* nav, const char* title, const char* line1, const char* line2,
    const seedtool_progress_t* progress)
{
    seedtool_render_nav_dice(nav, title, line1, line2, progress);
    present();
}

void seedtool_display_keyboard(const char* title, const char* text, const char* layout, const bool* enabled,
    const size_t selected, const size_t position, const size_t total)
{
    seedtool_render_keyboard(title, text, layout, enabled, selected, position, total);
    present();
}

bool seedtool_display_qr(const char* title, const char* text)
{
    const bool ok = seedtool_render_qr(title, text);
    if (ok) {
        present();
    }
    return ok;
}

bool seedtool_display_qr_address(const char* title, const char* text, const size_t selected)
{
    const bool ok = seedtool_render_qr_address(title, text, selected);
    if (ok) {
        present();
    }
    return ok;
}

bool seedtool_display_qr_bytes(const char* title, const uint8_t* data, const size_t len)
{
    const bool ok = seedtool_render_qr_bytes(title, data, len);
    if (ok) {
        present();
    }
    return ok;
}

bool seedtool_display_qr_bytes_region(const char* title, const uint8_t* data, const size_t len, const size_t region_index)
{
    const bool ok = seedtool_render_qr_bytes_region(title, data, len, region_index);
    if (ok) {
        present();
    }
    return ok;
}

bool seedtool_display_qr_bytes_map(const char* title, const uint8_t* data, const size_t len)
{
    const bool ok = seedtool_render_qr_bytes_map(title, data, len);
    if (ok) {
        present();
    }
    return ok;
}

void seedtool_display_stackbit_screen(const seedtool_nav_t* nav, const char* title, const unsigned word_number,
    const char* word, const char* footer)
{
    seedtool_render_stackbit_screen(nav, title, word_number, word, footer);
    present();
}

void seedtool_display_stackbit_physical_screen(const seedtool_nav_t* nav, const char* title,
    const unsigned word_number, const char* word, const char* footer)
{
    seedtool_render_stackbit_physical_screen(nav, title, word_number, word, footer);
    present();
}

void seedtool_platform_init(void)
{
    seedtool_display_init();
    puts("Controls: Left/A or Up = previous; Right/D or Down = next; Enter/Space, or "
         "Left+Right together as on the device = select; Q/Esc = quit");
}

uint64_t seedtool_platform_milliseconds(void) { return SDL_GetTicks64(); }

/* The device has no select button: pressing both navigation buttons together
 * stands in for it, so the simulator accepts that chord as well as Enter. */
static bool other_button_down(const SDL_Keycode pressed)
{
    const Uint8* const keys = SDL_GetKeyboardState(NULL);
    const bool left_down = keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A];
    const bool right_down = keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D];
    return (pressed == SDLK_LEFT || pressed == SDLK_a) ? right_down : left_down;
}

seedtool_key_t seedtool_platform_wait_key(const uint32_t timeout_ms)
{
    const uint64_t deadline = SDL_GetTicks64() + timeout_ms;
    for (;;) {
        const uint64_t now = SDL_GetTicks64();
        if (now >= deadline) {
            return KEY_TIMEOUT;
        }
        const uint64_t remaining = deadline - now;
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, remaining > INT32_MAX ? INT32_MAX : (int)remaining)) {
            continue;
        }
        if (event.type == SDL_QUIT) {
            seedtool_platform_restart();
        }
        if (event.type != SDL_KEYDOWN) {
            continue;
        }
        const SDL_Keycode key = event.key.keysym.sym;
        switch (key) {
        case SDLK_LEFT:
        case SDLK_a:
            return other_button_down(key) ? KEY_SELECT : KEY_PREV;
        case SDLK_RIGHT:
        case SDLK_d:
            return other_button_down(key) ? KEY_SELECT : KEY_NEXT;
        case SDLK_UP:
            /* Plain navigation aliases: unlike Left/Right, pressing both
             * together is not wired to a chord, since the device itself has no
             * up/down buttons for this to stand in for. */
            return KEY_PREV;
        case SDLK_DOWN:
            return KEY_NEXT;
        case SDLK_RETURN:
        case SDLK_SPACE:
            if (!event.key.repeat) {
                return KEY_SELECT;
            }
            break;
        case SDLK_ESCAPE:
        case SDLK_q:
            seedtool_platform_restart();
        default:
            break;
        }
    }
}

/* The simulator reads key events rather than pin levels, so it holds no press
 * to abandon; what stands in for one is the queue, where a key struck before
 * the new screen appeared is still waiting to be delivered to it. */
void seedtool_platform_flush_keys(void)
{
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_KEYDOWN);
}

void seedtool_platform_random(uint8_t* output, const size_t output_len)
{
    size_t offset = 0;
    while (offset < output_len) {
        const ssize_t result = getrandom(output + offset, output_len - offset, 0);
        if (result > 0) {
            offset += (size_t)result;
        } else if (result < 0 && errno != EINTR) {
            perror("getrandom");
            exit(EXIT_FAILURE);
        }
    }
}

_Noreturn void seedtool_platform_restart(void)
{
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    exit(EXIT_SUCCESS);
}
