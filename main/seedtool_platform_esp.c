#include "seedtool_platform.h"

#include "seedtool_display.h"

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>

/* These are read against a 100 Hz FreeRTOS tick (CONFIG_FREERTOS_HZ), so every
 * delay below rounds down to a whole 10 ms tick: POLL_MS actually polls every
 * 10 ms, which leaves the release settling in 10 ms rather than the 15 it reads
 * as. The hold values are multiples of 10 and so come out exact. The short
 * settle is left alone deliberately, since bounce cannot double-fire here: the
 * press latch below is only cleared once a release has been committed, so a
 * contact dropping back mid-release just restarts the settle. Anyone raising
 * the tick rate, or tuning these against a stopwatch, should know the two do
 * not currently agree. */
#define POLL_MS 15
#define SETTLE_POLLS 2
#define HOLD_DELAY_MS 500
#define HOLD_REPEAT_MS 140
#define BUTTON_LEFT_GPIO 35
#define BUTTON_RIGHT_GPIO 0

/* Press state persists across calls so that a held button keeps repeating and
 * so that a button released while the other is still down cannot be mistaken
 * for a navigation event. Both-buttons is only resolved once both are up. */
static bool left_seen, right_seen, repeated;
static uint64_t repeat_at;
static unsigned idle_polls;
/* Set by seedtool_platform_flush_keys, cleared once both buttons are up. */
static bool discarding;

static bool pressed(const gpio_num_t pin) { return gpio_get_level(pin) == 0; }

void seedtool_platform_init(void)
{
    /* Both buttons are wired to ground, so an idle pin has to be held high from
     * somewhere. GPIO0 is a strapping pin: the ROM leaves its internal pull-up
     * on, which is why boot and the splash are fine, but configuring the pin
     * without asking for that pull-up takes it away and leaves GPIO0 floating
     * down to ground, reading as a button held down forever. GPIO35 is
     * input-only and has no internal pull resistors at all, so it keeps
     * depending on the board's external pull-up; asking for one here is a no-op
     * on that pin. */
    const gpio_config_t buttons
        = { .pin_bit_mask = (UINT64_C(1) << BUTTON_LEFT_GPIO) | (UINT64_C(1) << BUTTON_RIGHT_GPIO),
              .mode = GPIO_MODE_INPUT,
              .pull_up_en = GPIO_PULLUP_ENABLE,
              .pull_down_en = GPIO_PULLDOWN_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&buttons));
    seedtool_display_init();
}

uint64_t seedtool_platform_milliseconds(void) { return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS; }

seedtool_key_t seedtool_platform_wait_key(const uint32_t timeout_ms)
{
    const uint64_t start = seedtool_platform_milliseconds();
    for (;;) {
        const uint64_t now = seedtool_platform_milliseconds();
        const bool left = pressed(BUTTON_LEFT_GPIO);
        const bool right = pressed(BUTTON_RIGHT_GPIO);

        if (discarding) {
            /* Zeroing the latch alone would not be enough while a button is
             * still down: the press would simply be counted from here and its
             * release would act on the new screen anyway. Stay silent until the
             * user has let go of everything. */
            discarding = left || right;
        } else if (left || right) {
            idle_polls = 0;
            if (!left_seen && !right_seen) {
                repeated = false;
                repeat_at = now + HOLD_DELAY_MS;
            }
            left_seen = left_seen || left;
            right_seen = right_seen || right;
            /* Auto-repeat navigation only; holding both buttons must not repeat
             * the select action. */
            if (left_seen != right_seen && now >= repeat_at) {
                repeated = true;
                repeat_at = now + HOLD_REPEAT_MS;
                return left_seen ? KEY_PREV : KEY_NEXT;
            }
        } else if (left_seen || right_seen) {
            if (++idle_polls < SETTLE_POLLS) {
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                continue;
            }
            const bool both = left_seen && right_seen;
            const bool was_left = left_seen;
            const bool was_repeated = repeated;
            left_seen = right_seen = repeated = false;
            idle_polls = 0;
            if (both) {
                return KEY_SELECT;
            }
            if (!was_repeated) {
                return was_left ? KEY_PREV : KEY_NEXT;
            }
        }

        if (now - start >= timeout_ms) {
            return KEY_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void seedtool_platform_flush_keys(void)
{
    left_seen = right_seen = repeated = false;
    idle_polls = 0;
    discarding = true;
}

void seedtool_platform_random(uint8_t* output, const size_t output_len) { esp_fill_random(output, output_len); }

_Noreturn void seedtool_platform_restart(void)
{
    esp_restart();
    __builtin_unreachable();
}

void __wrap_abort(void) { seedtool_platform_restart(); }
