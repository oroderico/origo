#include "seedtool_core.h"

#include <driver/gpio.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <wally_bip39.h>
#include <wally_core.h>

#include "seedtool_display.h"

#define SESSION_TIMEOUT_MS (10 * 60 * 1000)
#define WARNING_TIMEOUT_MS (60 * 1000)
#define POLL_MS 20

typedef enum { KEY_LEFT, KEY_RIGHT, KEY_TIMEOUT } key_t;

static TickType_t last_action;

static void seedtool_require(const bool condition)
{
    if (!condition) {
        esp_restart();
    }
}

void __wrap_abort(void)
{
    esp_restart();
    __builtin_unreachable();
}

static void screen_text(const char* title, const char* line1, const char* line2, const char* footer)
{
    seedtool_display_screen(title, line1, line2, footer);
}

static bool pressed(const gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

static key_t wait_key_raw(const uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < timeout_ms) {
        if (pressed(SEEDTOOL_BUTTON_LEFT_GPIO) || pressed(SEEDTOOL_BUTTON_RIGHT_GPIO)) {
            const key_t key = pressed(SEEDTOOL_BUTTON_LEFT_GPIO) ? KEY_LEFT : KEY_RIGHT;
            while (pressed(SEEDTOOL_BUTTON_LEFT_GPIO) || pressed(SEEDTOOL_BUTTON_RIGHT_GPIO)) {
                vTaskDelay(POLL_MS / portTICK_PERIOD_MS);
            }
            last_action = xTaskGetTickCount();
            return key;
        }
        vTaskDelay(POLL_MS / portTICK_PERIOD_MS);
    }
    return KEY_TIMEOUT;
}

static key_t wait_key(void)
{
    for (;;) {
        const uint32_t idle_ms = (xTaskGetTickCount() - last_action) * portTICK_PERIOD_MS;
        if (idle_ms < SESSION_TIMEOUT_MS) {
            const key_t key = wait_key_raw(SESSION_TIMEOUT_MS - idle_ms);
            if (key != KEY_TIMEOUT) {
                return key;
            }
        }
        screen_text("Session timeout", "Secrets will be erased", "in 60 seconds", "LEFT=erase RIGHT=extend");
        const key_t key = wait_key_raw(WARNING_TIMEOUT_MS);
        if (key != KEY_RIGHT) {
            return KEY_TIMEOUT;
        }
    }
}

static bool acknowledge(const char* title, const char* one, const char* two)
{
    screen_text(title, one, two, "LEFT=back RIGHT=continue");
    return wait_key() == KEY_RIGHT;
}

static int choose(const char* title, const char* const* items, const size_t count)
{
    size_t selected = 0;
    for (;;) {
        char pos[32];
        (void)snprintf(pos, sizeof(pos), "%u/%u", (unsigned)(selected + 1), (unsigned)count);
        screen_text(title, items[selected], pos, "LEFT=next RIGHT=select");
        const key_t key = wait_key();
        if (key == KEY_TIMEOUT) {
            return -1;
        }
        if (key == KEY_RIGHT) {
            return (int)selected;
        }
        selected = (selected + 1) % count;
    }
}

static bool enter_value(const char* title, const unsigned position, const unsigned total, const unsigned min,
    const unsigned max, unsigned* value)
{
    unsigned current = min;
    for (;;) {
        char line1[48], line2[48];
        (void)snprintf(line1, sizeof(line1), "Entry %u of %u", position, total);
        (void)snprintf(line2, sizeof(line2), "%u", current);
        screen_text(title, line1, line2, "LEFT=change RIGHT=accept");
        const key_t key = wait_key();
        if (key == KEY_TIMEOUT) {
            return false;
        }
        if (key == KEY_RIGHT) {
            *value = current;
            return true;
        }
        current = current == max ? min : current + 1;
    }
}

static void hexstr(const uint8_t* bytes, const size_t len, char* output)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        output[i * 2] = hex[bytes[i] >> 4];
        output[i * 2 + 1] = hex[bytes[i] & 15];
    }
    output[len * 2] = '\0';
}

static bool page_text(const char* title, const char* text, const size_t width)
{
    const size_t len = strlen(text);
    for (size_t offset = 0; offset < len; offset += width) {
        char page[97];
        const size_t n = len - offset < width ? len - offset : width;
        memcpy(page, text + offset, n);
        page[n] = '\0';
        char counter[32];
        (void)snprintf(counter, sizeof(counter), "%u/%u", (unsigned)(offset / width + 1),
            (unsigned)((len + width - 1) / width));
        screen_text(title, page, counter, "LEFT=exit RIGHT=next");
        const key_t key = wait_key();
        if (key != KEY_RIGHT) {
            return false;
        }
    }
    return true;
}

static void show_qr(const char* value)
{
    if (seedtool_display_qr(value)) {
        (void)wait_key();
    }
}

static bool enter_passphrase_once(char* output, const size_t output_len)
{
    size_t used = 0;
    unsigned selected = 0;
    output[0] = '\0';
    for (;;) {
        char visible[48];
        const char* tail = used > 32 ? output + used - 32 : output;
        (void)snprintf(visible, sizeof(visible), "%s", tail);
        char choice[32];
        if (selected == 95) {
            strcpy(choice, "<DONE>");
        } else if (selected == 96) {
            strcpy(choice, "<BACKSPACE>");
        } else {
            (void)snprintf(choice, sizeof(choice), "character: %c", (char)(0x20 + selected));
        }
        screen_text("BIP39 passphrase", visible, choice, "LEFT=next RIGHT=select");
        const key_t key = wait_key();
        if (key == KEY_TIMEOUT) {
            return false;
        }
        if (key == KEY_LEFT) {
            selected = (selected + 1) % 97;
        } else if (selected == 95) {
            return true;
        } else if (selected == 96) {
            if (used) {
                output[--used] = '\0';
            }
        } else if (used + 1 < output_len) {
            output[used++] = (char)(0x20 + selected);
            output[used] = '\0';
        }
    }
}

static bool get_session_passphrase(char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1])
{
    const char* options[] = { "No passphrase", "Enter passphrase" };
    const int selected = choose("Optional passphrase", options, 2);
    if (selected <= 0) {
        passphrase[0] = '\0';
        return selected == 0;
    }
    char confirmation[SEEDTOOL_MAX_PASSPHRASE_LEN + 1];
    const bool ok = enter_passphrase_once(passphrase, SEEDTOOL_MAX_PASSPHRASE_LEN + 1)
        && acknowledge("Confirm passphrase", "Enter it a second time", "Exact match required")
        && enter_passphrase_once(confirmation, sizeof(confirmation)) && strcmp(passphrase, confirmation) == 0;
    seedtool_zero(confirmation, sizeof(confirmation));
    if (!ok) {
        seedtool_zero(passphrase, SEEDTOOL_MAX_PASSPHRASE_LEN + 1);
        (void)acknowledge("Passphrase mismatch", "Nothing was derived", "Try again");
    }
    return ok;
}

static void show_wallet_data(const char* mnemonic)
{
    uint8_t fp[4];
    char fphex[9], address[SEEDTOOL_MAX_ADDRESS_LEN];
    char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1] = { 0 };
    if (!get_session_passphrase(passphrase)) {
        goto done;
    }
    if (seedtool_master_fingerprint(mnemonic, passphrase, fp) == SEEDTOOL_OK) {
        hexstr(fp, sizeof(fp), fphex);
        (void)acknowledge("Master fingerprint", fphex, passphrase[0] ? "Passphrase: session only" : "Passphrase: none");
    }
    for (uint32_t index = 0; index < 100;) {
        const char* types[] = { "BIP84 (bc1q)", "BIP86 (bc1p)", "Change index", "Done / erase" };
        const int selected = choose("Receive addresses", types, 4);
        if (selected < 0 || selected == 3) {
            break;
        }
        if (selected == 2) {
            unsigned chosen;
            if (enter_value("Address index", index + 1, 100, 0, 99, &chosen)) {
                index = chosen;
            }
            continue;
        }
        const seedtool_address_type_t type = selected == 0 ? SEEDTOOL_BIP84 : SEEDTOOL_BIP86;
        if (seedtool_mainnet_address(mnemonic, passphrase, type, index, address, sizeof(address)) == SEEDTOOL_OK) {
            char path[32];
            (void)snprintf(path, sizeof(path), "m/%u'/0'/0'/0/%u", (unsigned)type, (unsigned)index);
            if (page_text(path, address, 32) && acknowledge("Address QR", "Raw address only", "No xpub / no metadata")) {
                show_qr(address);
            }
        }
        seedtool_zero(address, sizeof(address));
    }
done:
    seedtool_zero(fp, sizeof(fp));
    seedtool_zero(fphex, sizeof(fphex));
    seedtool_zero(address, sizeof(address));
    seedtool_zero(passphrase, sizeof(passphrase));
}

static void show_generated(seedtool_generated_t* generated)
{
    char hash[65];
    hexstr(generated->hash, sizeof(generated->hash), hash);
    if (page_text("Canonical transcript", generated->transcript, 72) && page_text("SHA256", hash, 32)
        && page_text("BIP39 mnemonic", generated->mnemonic, 64)) {
        show_wallet_data(generated->mnemonic);
    }
    seedtool_zero(hash, sizeof(hash));
}

static void create_seed(void)
{
    const char* sources[] = { "D6 dice", "D20 dice", "Coin flips", "Cards" };
    const int source = choose("Entropy source", sources, 4);
    if (source < 0) {
        return;
    }
    const char* lengths[] = { "12 words", "24 words" };
    const int length = source == SEEDTOOL_CARDS ? 0 : choose("Seed length", lengths, 2);
    if (length < 0) {
        return;
    }
    const size_t words = length ? 24 : 12;
    const size_t required = seedtool_required_events((seedtool_source_t)source, words);
    uint8_t values[256] = { 0 };
    bool used_cards[52] = { false };
    for (size_t i = 0; i < required; ++i) {
        unsigned value;
        const unsigned min = source == SEEDTOOL_COIN || source == SEEDTOOL_CARDS ? 0 : 1;
        const unsigned max = source == SEEDTOOL_D6 ? 6 : source == SEEDTOOL_D20 ? 20 : source == SEEDTOOL_COIN ? 1 : 51;
        do {
            if (!enter_value(sources[source], i + 1, required, min, max, &value)) {
                seedtool_zero(values, sizeof(values));
                return;
            }
        } while (source == SEEDTOOL_CARDS && used_cards[value]
            && acknowledge("Duplicate card", "Card already entered", "Choose another"));
        if (source == SEEDTOOL_CARDS && used_cards[value]) {
            seedtool_zero(values, sizeof(values));
            return;
        }
        values[i] = value;
        if (source == SEEDTOOL_CARDS) {
            used_cards[value] = true;
        }
    }
    seedtool_generated_t generated;
    if (seedtool_generate((seedtool_source_t)source, words, values, required, &generated) == SEEDTOOL_OK) {
        show_generated(&generated);
    } else {
        (void)acknowledge("Error", "Could not generate seed", NULL);
    }
    seedtool_zero(values, sizeof(values));
    seedtool_zero(used_cards, sizeof(used_cards));
    seedtool_zero(&generated, sizeof(generated));
}

static bool enter_word_indices(const size_t count, char* mnemonic, const size_t mnemonic_len)
{
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        unsigned index;
        if (!enter_value("BIP39 word index", i + 1, count, 0, 2047, &index)) {
            return false;
        }
        const char* word = bip39_get_word_by_index(NULL, index);
        const size_t n = strlen(word);
        if (used + n + (i ? 1 : 0) + 1 > mnemonic_len) {
            return false;
        }
        if (i) {
            mnemonic[used++] = ' ';
        }
        memcpy(mnemonic + used, word, n + 1);
        used += n;
    }
    return true;
}

static void complete_checksum(void)
{
    const char* lengths[] = { "11 words + 7 coins", "23 words + 3 coins" };
    const int selected = choose("Complete checksum", lengths, 2);
    if (selected < 0) {
        return;
    }
    const size_t count = selected ? 23 : 11;
    const size_t bits_count = selected ? 3 : 7;
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    uint8_t bits[7] = { 0 };
    if (!enter_word_indices(count, prefix, sizeof(prefix))) {
        goto done;
    }
    for (size_t i = 0; i < bits_count; ++i) {
        unsigned bit;
        if (!enter_value("Coin: 0=Tails 1=Heads", i + 1, bits_count, 0, 1, &bit)) {
            goto done;
        }
        bits[i] = bit;
    }
    if (seedtool_complete_checksum(prefix, bits, bits_count, completed, sizeof(completed)) == SEEDTOOL_OK
        && page_text("Completed mnemonic", completed, 64)) {
        show_wallet_data(completed);
    }
done:
    seedtool_zero(prefix, sizeof(prefix));
    seedtool_zero(completed, sizeof(completed));
    seedtool_zero(bits, sizeof(bits));
}

static void verify_seed(void)
{
    const char* lengths[] = { "12 words", "24 words" };
    const int selected = choose("Verify mnemonic", lengths, 2);
    if (selected < 0) {
        return;
    }
    char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    if (enter_word_indices(selected ? 24 : 12, mnemonic, sizeof(mnemonic))) {
        if (seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK) {
            if (acknowledge("Checksum valid", "BIP39 English", "Derivation unlocked")) {
                show_wallet_data(mnemonic);
            }
        } else {
            (void)acknowledge("INVALID CHECKSUM", "Addresses are blocked", "Check every word");
        }
    }
    seedtool_zero(mnemonic, sizeof(mnemonic));
}

void app_main(void)
{
    gpio_config_t buttons = { .pin_bit_mask = (1ULL << SEEDTOOL_BUTTON_LEFT_GPIO)
            | (1ULL << SEEDTOOL_BUTTON_RIGHT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE };
    ESP_ERROR_CHECK(gpio_config(&buttons));
    seedtool_display_init();
    seedtool_require(wally_init(0) == WALLY_OK);

    /* This RNG is exclusively secp256k1 side-channel blinding. Core output
     * generation has no RNG parameter and remains invariant if this is stubbed. */
    uint8_t blinding[WALLY_SECP_RANDOMIZE_LEN];
    esp_fill_random(blinding, sizeof(blinding));
    seedtool_require(wally_secp_randomize(blinding, sizeof(blinding)) == WALLY_OK);
    seedtool_zero(blinding, sizeof(blinding));
    last_action = xTaskGetTickCount();

    if (!acknowledge("JADE SEED TOOL", "OFFLINE / STATELESS", "User entropy only")) {
        esp_restart();
    }
    for (;;) {
        const char* menu[] = { "Create Seed", "Complete Checksum", "Verify Existing Seed", "About / Safety" };
        const int selected = choose("Origo", menu, 4);
        if (selected < 0) {
            esp_restart();
        } else if (selected == 0) {
            create_seed();
        } else if (selected == 1) {
            complete_checksum();
        } else if (selected == 2) {
            verify_seed();
        } else {
            (void)page_text("Safety", "No seed is stored. No radio, wallet signing, PIN, OTA or serial RPC. Verify the firmware hash and record entropy independently.", 72);
        }
        last_action = xTaskGetTickCount();
    }
}
