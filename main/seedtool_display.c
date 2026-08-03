#include "seedtool_display.h"

#include "qrcode.h"

#include <driver/gpio.h>
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

#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_INVON 0x21
#define LCD_CMD_DISPON 0x29
#define LCD_CMD_CASET 0x2a
#define LCD_CMD_RASET 0x2b
#define LCD_CMD_RAMWR 0x2c
#define LCD_CMD_MADCTL 0x36
#define LCD_CMD_COLMOD 0x3a
#define LCD_CMD_RAMCTRL 0xb0

#define COLOR_BLACK UINT16_C(0x0000)
#define COLOR_WHITE UINT16_C(0xffff)

extern const unsigned char tft_DefaultFont[];
extern const unsigned char tft_Ubuntu16[];

typedef struct {
    uint8_t y_offset;
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    uint8_t x_advance;
    const uint8_t* bitmap;
} glyph_t;

/* Regular internal DRAM is DMA-capable on ESP32. Keeping this zero-initialized
 * array in BSS avoids embedding an empty 64.8 KiB framebuffer in the image. */
_Alignas(4) static uint16_t framebuffer[SEEDTOOL_DISPLAY_WIDTH * SEEDTOOL_DISPLAY_HEIGHT];
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

static void fill_rect(int x, int y, int width, int height, const uint16_t color)
{
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > SEEDTOOL_DISPLAY_WIDTH) {
        width = SEEDTOOL_DISPLAY_WIDTH - x;
    }
    if (y + height > SEEDTOOL_DISPLAY_HEIGHT) {
        height = SEEDTOOL_DISPLAY_HEIGHT - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        uint16_t* out = framebuffer + (y + row) * SEEDTOOL_DISPLAY_WIDTH + x;
        for (int col = 0; col < width; ++col) {
            out[col] = color;
        }
    }
}

static bool find_glyph(const uint8_t* font, const unsigned char character, glyph_t* glyph)
{
    const uint8_t* cursor = font + 4;
    while (*cursor != character && *cursor != 0xff) {
        const size_t bitmap_size = cursor[2] ? ((size_t)cursor[2] * cursor[3] + 7) / 8 : 0;
        cursor += 6 + bitmap_size;
    }
    if (*cursor == 0xff) {
        return false;
    }
    glyph->y_offset = cursor[1];
    glyph->width = cursor[2];
    glyph->height = cursor[3];
    glyph->x_offset = cursor[4] < 0x80 ? (int8_t)cursor[4] : (int8_t)(cursor[4] - 0x100);
    glyph->x_advance = cursor[5];
    glyph->bitmap = cursor + 6;
    return true;
}

static int glyph_advance(const uint8_t* font, const unsigned char character)
{
    glyph_t glyph;
    return find_glyph(font, character, &glyph) ? (glyph.width > glyph.x_advance ? glyph.width : glyph.x_advance) + 1 : 0;
}

static int text_width(const uint8_t* font, const char* text, const size_t length)
{
    int width = 0;
    for (size_t i = 0; i < length; ++i) {
        width += glyph_advance(font, (unsigned char)text[i]);
    }
    return width ? width - 1 : 0;
}

static void draw_glyph(const uint8_t* font, const unsigned char character, const int x, const int y)
{
    glyph_t glyph;
    if (!find_glyph(font, character, &glyph)) {
        return;
    }
    size_t bit = 0;
    for (uint8_t row = 0; row < glyph.height; ++row) {
        for (uint8_t col = 0; col < glyph.width; ++col, ++bit) {
            if (glyph.bitmap[bit / 8] & (UINT8_C(0x80) >> (bit % 8))) {
                fill_rect(x + glyph.x_offset + col, y + glyph.y_offset + row, 1, 1, COLOR_WHITE);
            }
        }
    }
}

static void draw_centered_line(const uint8_t* font, const char* text, const size_t length, const int y)
{
    int x = (SEEDTOOL_DISPLAY_WIDTH - text_width(font, text, length)) / 2;
    if (x < 0) {
        x = 0;
    }
    for (size_t i = 0; i < length; ++i) {
        draw_glyph(font, (unsigned char)text[i], x, y);
        x += glyph_advance(font, (unsigned char)text[i]);
    }
}

static void draw_centered(const uint8_t* font, const char* text, int y)
{
    if (!text) {
        return;
    }
    const int line_height = font[1];
    const char* cursor = text;
    while (*cursor && y + line_height <= SEEDTOOL_DISPLAY_HEIGHT) {
        const char* end = cursor;
        int width = 0;
        while (*end && *end != '\n') {
            const int advance = glyph_advance(font, (unsigned char)*end);
            if (end != cursor && width + advance > SEEDTOOL_DISPLAY_WIDTH) {
                break;
            }
            width += advance;
            ++end;
        }
        draw_centered_line(font, cursor, (size_t)(end - cursor), y);
        cursor = *end == '\n' ? end + 1 : end;
        y += line_height;
    }
}

static void flush(void)
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
    transmit(framebuffer, sizeof(framebuffer));
}

void seedtool_display_init(void)
{
    const gpio_config_t control_pins = {
        .pin_bit_mask = (UINT64_C(1) << LCD_PIN_DC) | (UINT64_C(1) << LCD_PIN_RST)
            | (UINT64_C(1) << LCD_PIN_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&control_pins));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 0));
    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_CLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = sizeof(framebuffer),
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
    const uint8_t orientation = 0x60; /* mirror X and swap X/Y */
    const uint8_t color_mode = 0x55;  /* RGB565 */
    const uint8_t ram_control[] = { 0x00, 0xf0 }; /* big-endian pixel stream */
    lcd_command(LCD_CMD_MADCTL, &orientation, sizeof(orientation));
    lcd_command(LCD_CMD_COLMOD, &color_mode, sizeof(color_mode));
    lcd_command(LCD_CMD_RAMCTRL, ram_control, sizeof(ram_control));
    lcd_command(LCD_CMD_INVON, NULL, 0);
    lcd_command(LCD_CMD_DISPON, NULL, 0);
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BACKLIGHT, 1));
    fill_rect(0, 0, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT, COLOR_BLACK);
    flush();
}

void seedtool_display_screen(const char* title, const char* line1, const char* line2, const char* footer)
{
    fill_rect(0, 0, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT, COLOR_BLACK);
    draw_centered(tft_Ubuntu16, title, 5);
    draw_centered(tft_DefaultFont, line1, 39);
    draw_centered(tft_DefaultFont, line2, 65);
    draw_centered(tft_DefaultFont, footer, 111);
    flush();
}

bool seedtool_display_qr(const char* text)
{
    uint8_t modules[qrcode_getBufferSize(5)];
    QRCode qr;
    if (qrcode_initText(&qr, modules, 5, ECC_LOW, text) != 0) {
        return false;
    }
    const int scale = 3;
    const int extent = (qr.size + 2) * scale;
    const int left = (SEEDTOOL_DISPLAY_WIDTH - extent) / 2;
    const int top = (SEEDTOOL_DISPLAY_HEIGHT - extent) / 2;
    fill_rect(0, 0, SEEDTOOL_DISPLAY_WIDTH, SEEDTOOL_DISPLAY_HEIGHT, COLOR_BLACK);
    fill_rect(left, top, extent, extent, COLOR_WHITE);
    for (uint8_t y = 0; y < qr.size; ++y) {
        for (uint8_t x = 0; x < qr.size; ++x) {
            if (qrcode_getModule(&qr, x, y)) {
                fill_rect(left + (x + 1) * scale, top + (y + 1) * scale, scale, scale, COLOR_BLACK);
            }
        }
    }
    flush();
    memset(modules, 0, sizeof(modules));
    return true;
}
