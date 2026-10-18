/* The app driven by a scripted key sequence instead of a window: a platform and
 * display backend that reads keys from argv[1] and prints a hash of every
 * frame the app shows. Two builds that print the same lines for the same
 * script drew the same screens, which is what makes a refactor of the app
 * checkable without a person pressing buttons.
 *
 *   origo-replay <keys>    p = previous, n = next, s = select, t = timeout
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
static uint64_t clock_ms;

void seedtool_display_init(void) { }

void seedtool_display_set_orientation(const bool flipped) { printf("orientation %d\n", flipped); }

void seedtool_display_set_brightness(const unsigned level)
{
    printf("brightness %u\n", seedtool_display_clamp_brightness(level));
}

void seedtool_display_flush(void)
{
    const uint16_t* const pixels = seedtool_render_pixels();
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < (size_t)SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT; ++i) {
        hash = (hash ^ pixels[i]) * 0x100000001b3ULL;
    }
    printf("frame %016llx\n", (unsigned long long)hash);
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
    if (argc != 2 || strspn(argv[1], "pnst") != strlen(argv[1])) {
        fprintf(stderr, "usage: %s <keys: p n s t>\n", argv[0]);
        return 2;
    }
    script = argv[1];
    seedtool_run();
    return 0;
}
