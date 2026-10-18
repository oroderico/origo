/* The top level: the Origo menu, settings, and seedtool_run. */
#include "app_internal.h"

#include <stdio.h>
#include <wally_core.h>

static void format_brightness(char* const buf, const size_t buf_len)
{
    (void)snprintf(buf, buf_len, "%u/%u", backlight_level, SEEDTOOL_DISPLAY_BRIGHTNESS_MAX);
}

/* Applied live: KEY_PREV raises, KEY_NEXT lowers, the chord leaves. */
static void show_brightness(void)
{
    for (;;) {
        char shown[8];
        format_brightness(shown, sizeof(shown));
        screen_text("Brightness", shown, "", chord_learned ? NULL : NAV_FOOTER);
        switch (wait_key()) {
        case KEY_PREV:
            if (backlight_level < SEEDTOOL_DISPLAY_BRIGHTNESS_MAX) {
                seedtool_display_set_brightness(++backlight_level);
            }
            break;
        case KEY_NEXT:
            if (backlight_level > SEEDTOOL_DISPLAY_BRIGHTNESS_MIN) {
                seedtool_display_set_brightness(--backlight_level);
            }
            break;
        case KEY_REDRAW:
            break;
        default:
            return;
        }
    }
}

static void show_settings_menu(void)
{
    enum { SETTINGS_ORIENTATION, SETTINGS_BRIGHTNESS, SETTINGS_KEYBOARD, SETTINGS_ABOUT };
    size_t cursor = 0;
    for (;;) {
        char orientation_item[32], brightness_fraction[8], brightness_item[24];
        (void)snprintf(orientation_item, sizeof(orientation_item), "Flip Orientation: %s",
            orientation_flipped ? "On" : "Off");
        format_brightness(brightness_fraction, sizeof(brightness_fraction));
        (void)snprintf(brightness_item, sizeof(brightness_item), "Brightness: %s", brightness_fraction);
        const char* const keyboard_item = qwerty_keys ? "Keyboard: QWERTY" : "Keyboard: ABC";
        const char* items[] = {
            [SETTINGS_ORIENTATION] = orientation_item,
            [SETTINGS_BRIGHTNESS] = brightness_item,
            [SETTINGS_KEYBOARD] = keyboard_item,
            [SETTINGS_ABOUT] = "About",
        };
        const int selected = choose_menu_at("Settings", items, ARRAY_LEN(items), &cursor);
        if (selected == NAV_BACK) {
            return;
        }
        if (selected == SETTINGS_ORIENTATION) {
            orientation_flipped = !orientation_flipped;
            seedtool_display_set_orientation(orientation_flipped);
        } else if (selected == SETTINGS_BRIGHTNESS) {
            show_brightness();
        } else if (selected == SETTINGS_KEYBOARD) {
            /* Two arrangements and the row names the one in force, so it
             * toggles in place. */
            qwerty_keys = !qwerty_keys;
        } else {
            page_read("Safety",
                "No seed is stored. No radio, wallet signing, PIN, OTA or serial RPC. Verify the firmware hash and "
                "record entropy independently. The two buttons sit one above the other; top and bottom move, both "
                "together select. Origo is derived from parts of Blockstream Jade (github.com/Blockstream/Jade), "
                "not affiliated with or endorsed by "
                "Blockstream. Several UI elements - entropy quality, BBQr export, Compact SeedQR, Zoomed Regions "
                "and Stackbit 1248 - are adapted from Krux (github.com/selfcustody/krux).");
        }
    }
}

void seedtool_run(void)
{
    seedtool_platform_init();
    seedtool_require(wally_init(0) == WALLY_OK);

    /* This RNG is exclusively secp256k1 side-channel blinding. Core output
     * generation has no RNG parameter and remains invariant if this is stubbed. */
    uint8_t blinding[WALLY_SECP_RANDOMIZE_LEN];
    seedtool_platform_random(blinding, sizeof(blinding));
    seedtool_require(wally_secp_randomize(blinding, sizeof(blinding)) == WALLY_OK);
    seedtool_zero(blinding, sizeof(blinding));

    /* The logo holds the screen; presses are discarded, since it offers no
     * choice. */
    seedtool_render_splash();
    seedtool_display_flush();
    for (const uint64_t deadline = seedtool_platform_milliseconds() + SPLASH_MS;;) {
        const uint64_t now = seedtool_platform_milliseconds();
        if (now >= deadline) {
            break;
        }
        (void)seedtool_platform_wait_key((uint32_t)(deadline - now));
    }
    /* A button still held as the menu opens is dropped too. */
    seedtool_platform_flush_keys();
    last_action = seedtool_platform_milliseconds();

    size_t cursor = 0;
    for (;;) {
        const char* menu[] = { "New Seed", "Restore Seed", "Settings" };
        /* No arrow: this is the top. */
        const int selected
            = choose_nav("Origo", menu, sizeof(menu) / sizeof(menu[0]), NULL, false, false, &cursor);
        if (selected == 0) {
            show_new_seed_menu();
        } else if (selected == 1) {
            restore_seed();
        } else {
            show_settings_menu();
        }
        last_action = seedtool_platform_milliseconds();
    }
}
