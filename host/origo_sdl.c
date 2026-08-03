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

static _Noreturn void sdl_fail(const char* operation)
{
    fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    exit(EXIT_FAILURE);
}

static void present(void)
{
    if (SDL_UpdateTexture(texture, NULL, seedtool_render_pixels(), SEEDTOOL_DISPLAY_WIDTH * sizeof(uint16_t)) != 0) {
        sdl_fail("SDL_UpdateTexture");
    }
    SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
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

bool seedtool_display_qr(const char* text)
{
    const bool ok = seedtool_render_qr(text);
    if (ok) {
        present();
    }
    return ok;
}

void seedtool_platform_init(void)
{
    seedtool_display_init();
    puts("Controls: Left/A = next or back; Right/D/Enter/Space = select; Q/Esc = quit");
}

uint64_t seedtool_platform_milliseconds(void) { return SDL_GetTicks64(); }

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
        if (event.type != SDL_KEYDOWN || event.key.repeat) {
            continue;
        }
        switch (event.key.keysym.sym) {
        case SDLK_LEFT:
        case SDLK_a:
            return KEY_LEFT;
        case SDLK_RIGHT:
        case SDLK_d:
        case SDLK_RETURN:
        case SDLK_SPACE:
            return KEY_RIGHT;
        case SDLK_ESCAPE:
        case SDLK_q:
            seedtool_platform_restart();
        default:
            break;
        }
    }
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
