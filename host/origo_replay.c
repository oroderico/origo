/* The app driven by a scripted key sequence instead of a window: a platform and
 * display backend that reads keys from argv[1] and prints a hash of every
 * frame the app shows. Two builds that print the same lines for the same
 * script drew the same screens, which is what makes a refactor of the app
 * checkable without a person pressing buttons.
 *
 *   origo-replay <keys> [dir]    p = previous, n = next, s = select, t = timeout
 *
 * With `dir`, every frame is also written there as NNNN.ppm, so a change to
 * what the screens show can be looked at rather than only detected.
 *
 * Time is virtual: a key press moves the clock 100 ms, a timeout moves it to
 * the deadline the app asked for. Randomness is a fixed sequence. */
#include "seedtool_app.h"
#include "seedtool_display.h"
#include "seedtool_platform.h"
#include "seedtool_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* script;
static const char* frame_dir;
static unsigned frame_count;
static uint64_t clock_ms;

void seedtool_display_init(void) { }

void seedtool_display_set_orientation(const bool flipped) { printf("orientation %d\n", flipped); }

void seedtool_display_set_brightness(const unsigned level)
{
    printf("brightness %u\n", seedtool_display_clamp_brightness(level));
}

/* The frame as binary PPM, RGB565 widened to eight bits a channel. */
static void write_frame(const uint16_t* const pixels)
{
    char path[4096];
    (void)snprintf(path, sizeof(path), "%s/%04u.ppm", frame_dir, frame_count++);
    FILE* const out = fopen(path, "wb");
    if (!out) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    fprintf(out, "P6\n%d %d\n255\n", SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT);
    for (size_t i = 0; i < (size_t)SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT; ++i) {
        const unsigned r = (pixels[i] >> 11) & 0x1f, g = (pixels[i] >> 5) & 0x3f, b = pixels[i] & 0x1f;
        const unsigned char rgb[3] = { (unsigned char)(r * 255 / 31), (unsigned char)(g * 255 / 63),
            (unsigned char)(b * 255 / 31) };
        (void)fwrite(rgb, 1, sizeof(rgb), out);
    }
    if (fclose(out) != 0) {
        perror(path);
        exit(EXIT_FAILURE);
    }
}

void seedtool_display_flush(void)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < (size_t)SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT; ++i) {
        hash = (hash ^ pixels[i]) * 0x100000001b3ULL;
    }
    printf("frame %016llx\n", (unsigned long long)hash);
    if (frame_dir) {
        write_frame(pixels);
    }
}

void seedtool_platform_init(void) { seedtool_display_init(); }

uint64_t seedtool_platform_milliseconds(void) { return clock_ms; }

seedtool_key_t seedtool_platform_wait_key(const uint32_t timeout_ms)
{
    const char key = *script;
    if (!key) {
        puts("end");
        exit(EXIT_SUCCESS);
    }
    ++script;
    if (key == 't') {
        clock_ms += timeout_ms;
        return KEY_TIMEOUT;
    }
    clock_ms += 100;
    return key == 'p' ? KEY_PREV : key == 'n' ? KEY_NEXT : KEY_SELECT;
}

void seedtool_platform_flush_keys(void) { }

void seedtool_platform_random(uint8_t* output, const size_t output_len)
{
    for (size_t i = 0; i < output_len; ++i) {
        output[i] = (uint8_t)(i * 151 + 7);
    }
}

_Noreturn void seedtool_platform_restart(void)
{
    puts("restart");
    exit(EXIT_SUCCESS);
}

int main(const int argc, char** argv)
{
    if (argc < 2 || argc > 3 || strspn(argv[1], "pnst") != strlen(argv[1])) {
        fprintf(stderr, "usage: %s <keys: p n s t> [frame dir]\n", argv[0]);
        return 2;
    }
    script = argv[1];
    frame_dir = argc == 3 ? argv[2] : NULL;
    seedtool_run();
    return 0;
}
