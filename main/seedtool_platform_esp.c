#include "seedtool_platform.h"

#include "seedtool_display.h"

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>

#define POLL_MS 20
#define BUTTON_LEFT_GPIO 35
#define BUTTON_RIGHT_GPIO 0

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
    while (seedtool_platform_milliseconds() - start < timeout_ms) {
        if (pressed(BUTTON_LEFT_GPIO) || pressed(BUTTON_RIGHT_GPIO)) {
            const seedtool_key_t key = pressed(BUTTON_LEFT_GPIO) ? KEY_LEFT : KEY_RIGHT;
            while (pressed(BUTTON_LEFT_GPIO) || pressed(BUTTON_RIGHT_GPIO)) {
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            }
            return key;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    return KEY_TIMEOUT;
}

void seedtool_platform_random(uint8_t* output, const size_t output_len) { esp_fill_random(output, output_len); }

_Noreturn void seedtool_platform_restart(void)
{
    esp_restart();
    __builtin_unreachable();
}

void __wrap_abort(void) { seedtool_platform_restart(); }
