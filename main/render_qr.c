/* Every QR screen: a code with its title and controls, an address beside its
 * code, and Compact SeedQR whole, as a region map and region by region. Drawn
 * with the primitives in render_internal.h; the codes come from qrcode.c. */
#include "render_internal.h"

#include "qrcode.h"

#include <stdio.h>
#include <string.h>

/* Five greys for the QR's light half, near-black to white (16, 74, 124, 198,
 * 255 as RGB565). The ramp starts at white. */
static const uint16_t qr_shades[]
    = { UINT16_C(0x1082), UINT16_C(0x4a49), UINT16_C(0x7bef), UINT16_C(0xc618), COLOR_WHITE };
#define QR_SHADE_COUNT (sizeof(qr_shades) / sizeof(qr_shades[0]))
static size_t qr_shade_index = QR_SHADE_COUNT - 1;
static uint16_t qr_light(void) { return qr_shades[qr_shade_index]; }

void seedtool_render_qr_cycle_shade(void)
{
    qr_shade_index = qr_shade_index ? qr_shade_index - 1 : QR_SHADE_COUNT - 1;
}

/* Version 6 holds 134 bytes at ECC_LOW, enough for a key origin and an account
 * xpub in one code. Raising it is a compile error rather than a code clipped by
 * the display. */
#define QR_VERSION 6
#define QR_MAX_MODULES (17 + 4 * QR_VERSION)
#define QR_MARGIN 3
#define QR_TITLE_Y 50
_Static_assert(SEEDTOOL_DISPLAY_HEIGHT / (QR_MAX_MODULES + 2) >= 1, "the largest QR no longer fits the display");

/* Geometry for a square, height-bound grid of `modules` cells against the right
 * edge, with the title column to its left: a full code with its quiet zone, or
 * a zoomed region without one. */
typedef struct {
    int scale;
    int extent;
    int top;
    int code_x;
    int title_x;
    int title_width;
} qr_geometry_t;

/* The code sits against the right edge, and the title and the way out share the
 * margin to its left. */
static qr_geometry_t qr_geometry(const int modules)
{
    qr_geometry_t g;
    g.scale = SEEDTOOL_DISPLAY_HEIGHT / modules;
    g.extent = modules * g.scale;
    g.top = (SEEDTOOL_DISPLAY_HEIGHT - g.extent) / 2;
    g.code_x = SEEDTOOL_DISPLAY_WIDTH - QR_MARGIN - g.extent;
    g.title_x = QR_MARGIN;
    g.title_width = g.code_x - 2 * QR_MARGIN;
    return g;
}

/* The way out, drawn the way the header's controls are: filled when it is
 * where the cursor sits, outlined when it is not. */
static void draw_qr_back(const bool selected)
{
    /* Painted either way, not only when selected: a screen that redraws a
     * control to move the cursor off it has to cover the highlight it had. */
    fill_rect(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_BLACK);
    draw_border(NAV_BACK_X, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_DIM);
    draw_triangle(NAV_BACK_X + (NAV_BACK_WIDTH - NAV_ARROW_WIDTH) / 2,
        NAV_BACK_Y + (NAV_BACK_HEIGHT - NAV_ARROW_HEIGHT) / 2, NAV_ARROW_WIDTH, NAV_ARROW_HEIGHT, TRIANGLE_LEFT,
        selected ? COLOR_BLACK : COLOR_WHITE);
}

/* The shade control, against the code's left edge, since what it changes is the
 * code. A sun whose disc is drawn in the current shade, so it dims as the code
 * does; the rays and outline stay in ink so it stays visible. Drawn from
 * rectangles: the faces are ASCII only. */
#define QR_SHADE_GAP 3
#define QR_SUN_CORE 5
#define QR_SUN_GAP 2
#define QR_SUN_RAY 2
#define QR_SUN_EXTENT (QR_SUN_CORE + 2 * (QR_SUN_GAP + QR_SUN_RAY))

/* A code is never wider than the display is tall, so its left edge never comes
 * further left than this, and the sun stays clear of the way out. */
_Static_assert(SEEDTOOL_DISPLAY_WIDTH - QR_MARGIN - SEEDTOOL_DISPLAY_HEIGHT - QR_SHADE_GAP - NAV_BACK_WIDTH
        > NAV_BACK_X + NAV_BACK_WIDTH,
    "the QR's shade control would overlap its back arrow");

static void draw_qr_shade(const int box_x, const bool selected)
{
    fill_rect(box_x, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_BLACK);
    draw_border(box_x, NAV_BACK_Y, NAV_BACK_WIDTH, NAV_BACK_HEIGHT, selected ? COLOR_HIGHLIGHT : COLOR_DIM);

    const uint16_t ink = selected ? COLOR_BLACK : COLOR_WHITE;
    const int left = box_x + (NAV_BACK_WIDTH - QR_SUN_EXTENT) / 2;
    const int top = NAV_BACK_Y + (NAV_BACK_HEIGHT - QR_SUN_EXTENT) / 2;
    const int core_x = left + QR_SUN_GAP + QR_SUN_RAY;
    const int core_y = top + QR_SUN_GAP + QR_SUN_RAY;
    fill_rect(core_x, core_y, QR_SUN_CORE, QR_SUN_CORE, qr_light());
    draw_border(core_x - 1, core_y - 1, QR_SUN_CORE + 2, QR_SUN_CORE + 2, ink);

    /* One ray per side, each starting a gap out from the disc's outline. */
    const int middle = core_x + QR_SUN_CORE / 2;
    const int centre = core_y + QR_SUN_CORE / 2;
    fill_rect(middle, top, 1, QR_SUN_RAY, ink);
    fill_rect(middle, top + QR_SUN_EXTENT - QR_SUN_RAY, 1, QR_SUN_RAY, ink);
    fill_rect(left, centre, QR_SUN_RAY, 1, ink);
    fill_rect(left + QR_SUN_EXTENT - QR_SUN_RAY, centre, QR_SUN_RAY, 1, ink);
    /* And a dot on each diagonal, which is what stops four rays reading as a
     * cross rather than as a sun. */
    fill_rect(left + 1, top + 1, 1, 1, ink);
    fill_rect(left + QR_SUN_EXTENT - 2, top + 1, 1, 1, ink);
    fill_rect(left + 1, top + QR_SUN_EXTENT - 2, 1, 1, ink);
    fill_rect(left + QR_SUN_EXTENT - 2, top + QR_SUN_EXTENT - 2, 1, 1, ink);
}

/* Every QR screen has the same two controls: the way out at the top left and
 * the sun against the code. */
static void draw_qr_chrome(const int code_x, const size_t selected)
{
    draw_qr_back(selected == SEEDTOOL_NAV_BACK);
    draw_qr_shade(code_x - QR_SHADE_GAP - NAV_BACK_WIDTH, selected == SEEDTOOL_NAV_SHADE);
}

/* The code against the right edge inside its quiet zone, on a cleared screen:
 * the light field in the current shade, the dark modules over it. The scale is
 * the largest that fits the display's height. */
static qr_geometry_t draw_modules(const QRCode* qr)
{
    const qr_geometry_t g = qr_geometry(qr->size + 2);
    seedtool_render_clear();
    fill_rect(g.code_x, g.top, g.extent, g.extent, qr_light());
    for (uint8_t y = 0; y < qr->size; ++y) {
        for (uint8_t x = 0; x < qr->size; ++x) {
            if (qrcode_getModule((QRCode*)qr, x, y)) {
                fill_rect(g.code_x + (x + 1) * g.scale, g.top + (y + 1) * g.scale, g.scale, g.scale, COLOR_BLACK);
            }
        }
    }
    return g;
}

/* Draws the populated `qr` with its title in the margin, so a stepped sequence
 * of codes says what each holds, then zeroes `modules`, which held the encoded
 * value. */
static bool draw_qr(QRCode* qr, uint8_t* modules, const char* title, const size_t selected)
{
    const qr_geometry_t g = draw_modules(qr);
    draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, QR_TITLE_Y);
    draw_qr_chrome(g.code_x, selected);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
    return true;
}

bool seedtool_render_qr(const char* title, const char* text, const size_t selected)
{
    /* At the smallest version that holds `text`, so a short value gets coarser,
     * easier modules. */
    const uint8_t version = qrcode_versionForText(ECC_LOW, text, QR_VERSION);
    if (!version) {
        return false;
    }
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (qrcode_initText(&qr, modules, version, ECC_LOW, text) != 0) {
        /* A failed encode may already have written part of `text` into
         * `modules` before the encoder gave up. */
        memset(modules, 0, sizeof(modules));
        return false;
    }
    return draw_qr(&qr, modules, title, selected);
}

/* Two groups to a line, so an address always falls into the same block; a
 * 42-character bech32 address is five full lines and a short sixth. */
#define QR_ADDRESS_GROUPS 2
/* Between the path and the address: they are two different facts, and run
 * together they read as one wrapped string. */
#define QR_ADDRESS_GAP 5

/* The code, its derivation path and the address, on one screen: the way out,
 * then the path, then the address in groups of four, in the small face since it
 * is there to be checked, not transcribed. */
bool seedtool_render_qr_address(const char* title, const char* text, const size_t selected)
{
    const uint8_t version = qrcode_versionForText(ECC_LOW, text, QR_VERSION);
    if (!version) {
        return false;
    }
    const qr_geometry_t g = qr_geometry(17 + 4 * version + 2);

    /* Where the address starts: under the path as it will actually wrap, then
     * a gap that keeps the two facts apart without spending a whole line. */
    const int title_y = NAV_BACK_Y + NAV_BACK_HEIGHT + 6;
    const int text_top
        = layout_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, title_y, false) + QR_ADDRESS_GAP;
    const size_t total = strlen(text);

    /* A grid, not centred lines: the face is proportional, so each column is as
     * wide as its widest group and every group starts at its column's left
     * edge. */
    int column = 0;
    for (size_t i = 0; i < total; i += GROUP_LEN) {
        const size_t run = total - i < GROUP_LEN ? total - i : GROUP_LEN;
        int width = 0;
        for (size_t j = 0; j < run; ++j) {
            width += glyph_advance(tft_DefaultFont, (unsigned char)text[i + j]);
        }
        if (width > column) {
            column = width;
        }
    }
    const int block_width = QR_ADDRESS_GROUPS * column + (QR_ADDRESS_GROUPS - 1) * GROUP_GAP;

    /* Checked before anything is drawn, and why this returns a bool: an address
     * cut off at the margin looks like one that ended there. A taproot address
     * (62 characters) does not fit the 48 two columns hold. The last line is
     * measured to its lowest inked row, descenders included. */
    const int step = tft_DefaultFont[1] + 2;
    const size_t lines = (total + QR_ADDRESS_GROUPS * GROUP_LEN - 1) / (QR_ADDRESS_GROUPS * GROUP_LEN);
    const size_t last_line = (lines - 1) * QR_ADDRESS_GROUPS * GROUP_LEN;
    const int bottom
        = text_top + (int)(lines - 1) * step + text_ink_bottom(tft_DefaultFont, text + last_line, total - last_line);
    if (block_width > g.title_width || bottom > SEEDTOOL_DISPLAY_HEIGHT) {
        return false;
    }

    if (!seedtool_render_qr(NULL, text, selected)) {
        return false;
    }
    int y = draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, title_y) + QR_ADDRESS_GAP;

    int left = g.title_x + (g.title_width - block_width) / 2;
    if (left < g.title_x) {
        left = g.title_x;
    }

    for (size_t i = 0; i < total; i += GROUP_LEN) {
        const size_t run = total - i < GROUP_LEN ? total - i : GROUP_LEN;
        const size_t slot = (i / GROUP_LEN) % QR_ADDRESS_GROUPS;
        /* A last line that does not fill its columns is centred on the block. */
        const bool last_line = i + GROUP_LEN >= total && slot == 0;
        int x;
        if (last_line) {
            int width = 0;
            for (size_t j = 0; j < run; ++j) {
                width += glyph_advance(tft_DefaultFont, (unsigned char)text[i + j]);
            }
            x = left + (block_width - width) / 2;
        } else {
            x = left + (int)slot * (column + GROUP_GAP);
        }
        const uint16_t ink = group_ink(i / GROUP_LEN);
        for (size_t j = 0; j < run; ++j) {
            draw_glyph(tft_DefaultFont, (unsigned char)text[i + j], x, y, ink);
            x += glyph_advance(tft_DefaultFont, (unsigned char)text[i + j]);
        }
        if (slot + 1 == QR_ADDRESS_GROUPS) {
            y += tft_DefaultFont[1] + 2;
        }
    }
    return true;
}

size_t seedtool_render_qr_alphanumeric_capacity(const uint8_t max_version)
{
    size_t chars = 0;
    while (qrcode_versionForAlphanumeric(ECC_LOW, (uint16_t)(chars + 1), max_version)) {
        ++chars;
    }
    return chars;
}

/* Encodes `data` in byte mode at the smallest version that holds it: that is
 * what "compact" means in SeedSigner's convention. `modules` must hold
 * qrcode_getBufferSize(QR_VERSION), which bounds every smaller version. */
static bool encode_bytes(QRCode* qr, uint8_t* modules, const uint8_t* data, const size_t len)
{
    if (len > UINT16_MAX) {
        return false;
    }
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    /* qrcode_initBytes only reads through `data`; the cast does not let it
     * write through it. */
    if (!version || qrcode_initBytes(qr, modules, version, ECC_LOW, (uint8_t*)data, (uint16_t)len) != 0) {
        /* `data` is seed entropy, and a failed encode may already have written
         * some of it into `modules`. */
        memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
        return false;
    }
    return true;
}

bool seedtool_render_qr_bytes(const char* title, const uint8_t* data, const size_t len, const size_t selected)
{
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    return encode_bytes(&qr, modules, data, len) && draw_qr(&qr, modules, title, selected);
}

/* 7x7 blocks for the smallest (version 1, 21x21) QR, 5x5 otherwise: Krux's own
 * thresholds (src/krux/pages/qr_view.py, SeedQRView.__init__). */
static size_t qr_region_size(const uint8_t qr_size) { return qr_size == 21 ? 7 : 5; }

size_t seedtool_render_qr_bytes_regions(const size_t len)
{
    if (len > UINT16_MAX) {
        return 0;
    }
    const uint8_t version = qrcode_versionForBytes(ECC_LOW, (uint16_t)len, QR_VERSION);
    if (!version) {
        return 0;
    }
    const uint8_t qr_size = (uint8_t)(4 * version + 17);
    const size_t region_size = qr_region_size(qr_size);
    const size_t columns = ((size_t)qr_size + region_size - 1) / region_size;
    return columns * columns;
}

/* The region map: seedtool_render_qr_bytes's code, same geometry, with a dim
 * line at each region boundary and each region's label inside it, from the same
 * split draw_qr_region uses, so the map always matches the tiles. */
bool seedtool_render_qr_bytes_map(const char* title, const uint8_t* data, const size_t len, const size_t selected)
{
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (!encode_bytes(&qr, modules, data, len)) {
        return false;
    }
    const size_t region_size = qr_region_size(qr.size);
    const size_t columns = ((size_t)qr.size + region_size - 1) / region_size;
    const qr_geometry_t g = draw_modules(&qr);
    /* The code's top-left corner, one quiet-zone module in, which the grid and
     * labels are placed from. */
    const int code_left = g.code_x + g.scale;
    const int code_top = g.top + g.scale;
    const int code_extent = (int)qr.size * g.scale;
    const int block = (int)region_size * g.scale;
    for (size_t i = 0; i <= columns; ++i) {
        const int pos = (int)i * block;
        fill_rect(code_left, code_top + pos, code_extent, 1, COLOR_DIM);
        fill_rect(code_left + pos, code_top, 1, code_extent, COLOR_DIM);
    }
    /* A label must read over black and white modules alike, so each gets a
     * small white plate with black text. */
    for (size_t row = 0; row < columns; ++row) {
        for (size_t column = 0; column < columns; ++column) {
            char label[4];
            (void)snprintf(label, sizeof(label), "%c%u", (char)('A' + row), (unsigned)(column + 1));
            const int label_x = code_left + (int)column * block;
            const int label_y = code_top + (int)row * block + 2;
            const int label_height = tft_DefaultFont[1] + 2;
            fill_rect(label_x + 1, label_y - 1, block - 2, label_height, COLOR_WHITE);
            draw_centered_in(tft_DefaultFont, label, label_x, block, label_y, COLOR_BLACK);
        }
    }
    draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, QR_TITLE_Y);
    draw_qr_chrome(g.code_x, selected);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
    return true;
}

/* draw_qr's layout at a region's own scale: 5-7 cells rather than 21-25, so
 * each is much larger. No quiet zone: a region is copied by hand, never
 * scanned. */
static void draw_qr_region(QRCode* qr, uint8_t* modules, const char* title, const size_t region_size,
    const size_t columns, const size_t region_index, const size_t selected)
{
    const size_t row = region_index / columns;
    const size_t column = region_index % columns;
    const qr_geometry_t g = qr_geometry((int)region_size);
    seedtool_render_clear();
    for (size_t y = 0; y < region_size; ++y) {
        for (size_t x = 0; x < region_size; ++x) {
            const size_t qx = column * region_size + x;
            const size_t qy = row * region_size + y;
            const bool set = qx < qr->size && qy < qr->size && qrcode_getModule(qr, (uint8_t)qx, (uint8_t)qy);
            fill_rect(g.code_x + (int)x * g.scale, g.top + (int)y * g.scale, g.scale, g.scale,
                set ? COLOR_BLACK : qr_light());
        }
    }
    for (size_t i = 0; i <= region_size; ++i) {
        fill_rect(g.code_x, g.top + (int)i * g.scale, g.extent, 1, COLOR_DIM);
        fill_rect(g.code_x + (int)i * g.scale, g.top, 1, g.extent, COLOR_DIM);
    }
    const int label_y = draw_centered_box(tft_DefaultFont, title, g.title_x, g.title_width, QR_TITLE_Y);
    char label[24];
    (void)snprintf(label, sizeof(label), "Region %c%u", (char)('A' + row), (unsigned)(column + 1));
    draw_centered_box(tft_DefaultFont, label, g.title_x, g.title_width, label_y + 4);
    draw_qr_chrome(g.code_x, selected);
    memset(modules, 0, qrcode_getBufferSize(QR_VERSION));
}

bool seedtool_render_qr_bytes_region(
    const char* title, const uint8_t* data, const size_t len, const size_t region_index, const size_t selected)
{
    uint8_t modules[qrcode_getBufferSize(QR_VERSION)];
    QRCode qr;
    if (!encode_bytes(&qr, modules, data, len)) {
        return false;
    }
    const size_t region_size = qr_region_size(qr.size);
    const size_t columns = ((size_t)qr.size + region_size - 1) / region_size;
    if (region_index >= columns * columns) {
        memset(modules, 0, sizeof(modules));
        return false;
    }
    draw_qr_region(&qr, modules, title, region_size, columns, region_index, selected);
    return true;
}
