#include "seedtool_display.h"

#include "seedtool_render.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <string.h>

#define LCD_HOST SPI2_HOST
#define LCD_PIN_MOSI 19
#define LCD_PIN_CLK 18
#define LCD_PIN_CS 5
#define LCD_PIN_DC 16
#define LCD_PIN_RST 23
#define LCD_PIN_BACKLIGHT 4
#define LCD_CLOCK_HZ 32000000
#define LCD_GAP_X 40
#define LCD_GAP_Y 53

/* Backlight PWM through LEDC, with the timer, channel, resolution and
 * frequency of Jade's T-Display driver (jade/main/power/tdisplay.inc). */
#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_DUTY_MAX ((1 << LCD_BL_LEDC_DUTY_RES) - 1)
#define LCD_BL_LEDC_FREQUENCY_HZ 10000

#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_INVON 0x21
#define LCD_CMD_DISPON 0x29
#define LCD_CMD_CASET 0x2a
#define LCD_CMD_RASET 0x2b
#define LCD_CMD_RAMWR 0x2c
#define LCD_CMD_MADCTL 0x36
#define LCD_CMD_COLMOD 0x3a
#define LCD_CMD_RAMCTRL 0xb0

static spi_device_handle_t display;

static void transmit(const void* const data, const size_t length)
{
    if (!length) {
        return;
    }
    spi_transaction_t transaction = { .length = length * 8 };
    if (length <= sizeof(transaction.tx_data)) {
        transaction.flags = SPI_TRANS_USE_TXDATA;
        memcpy(transaction.tx_data, data, length);
    } else {
        transaction.tx_buffer = data;
    }
    ESP_ERROR_CHECK(spi_device_polling_transmit(display, &transaction));
}

static void lcd_command(const uint8_t command, const void* const data, const size_t length)
{
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_DC, 0));
    transmit(&command, sizeof(command));
    if (length) {
        ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_DC, 1));
        transmit(data, length);
    }
}

/* The panel reads each pixel most significant byte first, and the framebuffer
 * holds it the other way round because the CPU does. Converting a chunk at a
 * time keeps a second whole frame out of RAM and leaves what the renderer drew
 * untouched. */
#define FLUSH_ROWS 15
_Static_assert(SEEDTOOL_DISPLAY_HEIGHT % FLUSH_ROWS == 0, "the flush chunk must divide the display height");

static void transmit_pixels(void)
{
    static uint16_t wire[SEEDTOOL_DISPLAY_WIDTH * FLUSH_ROWS];
    for (size_t row = 0; row < SEEDTOOL_DISPLAY_HEIGHT; row += FLUSH_ROWS) {
        seedtool_render_wire_rows(wire, row, FLUSH_ROWS);
        transmit(wire, sizeof(wire));
    }
}

void seedtool_display_flush(void)
{
    const uint16_t x_start = LCD_GAP_X;
    const uint16_t x_end = LCD_GAP_X + SEEDTOOL_DISPLAY_WIDTH - 1;
    const uint16_t y_start = LCD_GAP_Y;
    const uint16_t y_end = LCD_GAP_Y + SEEDTOOL_DISPLAY_HEIGHT - 1;
    const uint8_t columns[] = { x_start >> 8, x_start & 0xff, x_end >> 8, x_end & 0xff };
    const uint8_t rows[] = { y_start >> 8, y_start & 0xff, y_end >> 8, y_end & 0xff };
    lcd_command(LCD_CMD_CASET, columns, sizeof(columns));
    lcd_command(LCD_CMD_RASET, rows, sizeof(rows));
    lcd_command(LCD_CMD_RAMWR, NULL, 0);
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_DC, 1));
    transmit_pixels();
}

static void backlight_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));
    const ledc_channel_config_t channel_config = {
        .gpio_num = LCD_PIN_BACKLIGHT,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
}

void seedtool_display_set_orientation(const bool flipped)
{
    const uint8_t madctl = flipped ? 0xa0 : 0x60;
    lcd_command(LCD_CMD_MADCTL, &madctl, sizeof(madctl));
}

void seedtool_display_set_brightness(const unsigned level)
{
    const unsigned clamped = seedtool_display_clamp_brightness(level);
    const uint32_t duty = clamped * LCD_BL_LEDC_DUTY_MAX / SEEDTOOL_DISPLAY_BRIGHTNESS_MAX;
    ESP_ERROR_CHECK(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL));
}

void seedtool_display_init(void)
{
    const gpio_config_t control_pins = {
        .pin_bit_mask = (UINT64_C(1) << LCD_PIN_DC) | (UINT64_C(1) << LCD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&control_pins));
    backlight_init();
    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_CLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = LCD_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = LCD_PIN_CS,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &device_config, &display));

    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_command(LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    const uint8_t color_mode = 0x55;
    const uint8_t ram_control[] = { 0x00, 0xf0 };
    seedtool_display_set_orientation(false);
    lcd_command(LCD_CMD_COLMOD, &color_mode, sizeof(color_mode));
    lcd_command(LCD_CMD_RAMCTRL, ram_control, sizeof(ram_control));
    lcd_command(LCD_CMD_INVON, NULL, 0);
    lcd_command(LCD_CMD_DISPON, NULL, 0);
    seedtool_display_set_brightness(SEEDTOOL_DISPLAY_BRIGHTNESS_DEFAULT);
    seedtool_render_clear();
    seedtool_display_flush();
}
