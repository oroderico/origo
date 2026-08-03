#include "seedtool_platform.h"

#include "seedtool_display.h"

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>

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

static bool pressed(const gpio_num_t pin) { return gpio_get_level(pin) == 0; }

void seedtool_platform_init(void)
{
    const gpio_config_t buttons
        = { .pin_bit_mask = (UINT64_C(1) << BUTTON_LEFT_GPIO) | (UINT64_C(1) << BUTTON_RIGHT_GPIO),
              .mode = GPIO_MODE_INPUT,
              .pull_up_en = GPIO_PULLUP_DISABLE,
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

        if (left || right) {
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

void seedtool_platform_random(uint8_t* output, const size_t output_len) { esp_fill_random(output, output_len); }

_Noreturn void seedtool_platform_restart(void)
{
    esp_restart();
    __builtin_unreachable();
}

void __wrap_abort(void) { seedtool_platform_restart(); }
