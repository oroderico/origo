#include "seedtool_core.h"

#include <stdio.h>
#include <string.h>
#include <wally_core.h>

#include "seedtool_display.h"
#include "seedtool_platform.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#define SESSION_TIMEOUT_MS (10 * 60 * 1000)
#define WARNING_TIMEOUT_MS (60 * 1000)
#define MAX_PAGE_LINES 24
#define MAX_LINE_CHARS 48
#define PASSPHRASE_TAIL 24

#define NAV_FOOTER "L/R move   BOTH select"
#define ACK_FOOTER "BOTH continue   L/R back"

/* Word entry keyboard. Key n below 26 is the letter 'a' + n, so the enabled
 * flags from the wordlist module index it directly; the last key is backspace. */
#define WORD_LAYOUT "abcdefghij\nklmnopqrs\ntuvwxyz\b"
#define WORD_KEYS (SEEDTOOL_LETTERS + 1)

#define PASSPHRASE_PAGES 4
static const char* const passphrase_layouts[PASSPHRASE_PAGES] = {
    "abcdefghij\nklmnopqrst\nuvwxyz \b\t\r",
    "ABCDEFGHIJ\nKLMNOPQRST\nUVWXYZ \b\t\r",
    "1234567890\n!\"#$%&'()\n*+,-./ \b\t\r",
    ":;<=>?@\n[\\]^_`~\n{|} \b\t\r",
};

typedef void (*format_fn)(unsigned value, char* output, size_t output_len);

static uint64_t last_action;

static void seedtool_require(const bool condition)
{
    if (!condition) {
        seedtool_platform_restart();
    }
}

static void screen_text(const char* title, const char* line1, const char* line2, const char* footer)
{
    seedtool_display_screen(title, line1, line2, footer);
}

static seedtool_key_t wait_key_raw(const uint32_t timeout_ms)
{
    const seedtool_key_t key = seedtool_platform_wait_key(timeout_ms);
    if (key != KEY_TIMEOUT) {
        last_action = seedtool_platform_milliseconds();
    }
    return key;
}

static seedtool_key_t wait_key(void)
{
    const uint64_t elapsed = seedtool_platform_milliseconds() - last_action;
    const uint32_t idle_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    if (idle_ms < SESSION_TIMEOUT_MS) {
        const seedtool_key_t key = wait_key_raw(SESSION_TIMEOUT_MS - idle_ms);
        if (key != KEY_TIMEOUT) {
            return key;
        }
    }
    screen_text("Session timeout", "Secrets will be erased", "in 60 seconds", "BOTH extend   L/R erase");
    /* The warning has replaced the caller's screen, so an extended session must
     * repaint it rather than let the next press act on what is no longer shown. */
    return wait_key_raw(WARNING_TIMEOUT_MS) == KEY_SELECT ? KEY_REDRAW : KEY_TIMEOUT;
}

static bool acknowledge(const char* title, const char* one, const char* two)
{
    for (;;) {
        screen_text(title, one, two, ACK_FOOTER);
        const seedtool_key_t key = wait_key();
        if (key != KEY_REDRAW) {
            return key == KEY_SELECT;
        }
    }
}

static int choose(const char* title, const char* const* items, const size_t count)
{
    size_t selected = 0;
    for (;;) {
        char footer[48];
        (void)snprintf(footer, sizeof(footer), "%u/%u   %s", (unsigned)(selected + 1), (unsigned)count, NAV_FOOTER);
        screen_text(title, items[selected], NULL, footer);
        switch (wait_key()) {
        case KEY_SELECT:
            return (int)selected;
        case KEY_PREV:
            selected = (selected + count - 1) % count;
            break;
        case KEY_NEXT:
            selected = (selected + 1) % count;
            break;
        case KEY_REDRAW:
            break;
        default:
            return -1;
        }
    }
}

static unsigned step_value(
    unsigned current, const unsigned min, const unsigned max, const bool forward, const bool* allowed)
{
    for (unsigned i = 0; i <= max - min; ++i) {
        current = forward ? (current == max ? min : current + 1) : (current == min ? max : current - 1);
        if (!allowed || allowed[current - min]) {
            return current;
        }
    }
    return current;
}

/* Numeric carousel. `allowed` is optional and indexed from `min`; disallowed
 * values are skipped, which is how already-drawn cards are kept out of reach.
 * A `total` of zero means this is a one-off value rather than one of a run. */
static bool enter_value(const char* title, const unsigned position, const unsigned total, const unsigned min,
    const unsigned max, unsigned* value, const format_fn format, const bool* allowed)
{
    unsigned current = min;
    if (allowed && !allowed[0]) {
        current = step_value(min, min, max, true, allowed);
    }
    for (;;) {
        char line1[48], line2[48];
        if (total) {
            (void)snprintf(line1, sizeof(line1), "Entry %u of %u", position, total);
        } else {
            (void)snprintf(line1, sizeof(line1), "Range %u to %u", min, max);
        }
        if (format) {
            format(current, line2, sizeof(line2));
        } else {
            (void)snprintf(line2, sizeof(line2), "%u", current);
        }
        screen_text(title, line2, line1, NAV_FOOTER);
        switch (wait_key()) {
        case KEY_SELECT:
            *value = current;
            return true;
        case KEY_PREV:
            current = step_value(current, min, max, false, allowed);
            break;
        case KEY_NEXT:
            current = step_value(current, min, max, true, allowed);
            break;
        case KEY_REDRAW:
            break;
        default:
            return false;
        }
    }
}

static void format_card(const unsigned value, char* output, const size_t output_len)
{
    static const char ranks[] = "A23456789TJQK";
    static const char suits[] = "CDHS";
    (void)snprintf(output, output_len, "%c%c", ranks[value % 13], suits[value / 13]);
}

static void format_coin(const unsigned value, char* output, const size_t output_len)
{
    (void)snprintf(output, output_len, "%s", value ? "Heads (1)" : "Tails (0)");
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

/* Two body lines per page, split by what actually fits on the display rather
 * than by a character count. Returns true when the reader advanced past the
 * last page or accepted, false when they backed out. */
static bool page_text(const char* title, const char* text)
{
    size_t start[MAX_PAGE_LINES], length[MAX_PAGE_LINES];
    const size_t total = strlen(text);
    size_t lines = 0, offset = 0;
    while (lines < MAX_PAGE_LINES && (offset < total || !lines)) {
        const size_t fit = seedtool_render_fit(text + offset, MAX_LINE_CHARS);
        start[lines] = offset;
        length[lines] = fit;
        /* A glyph wider than the display would otherwise loop forever. */
        offset += fit ? fit : 1;
        ++lines;
    }
    const size_t pages = (lines + 1) / 2;
    size_t page = 0;
    for (;;) {
        char line1[MAX_LINE_CHARS + 1], line2[MAX_LINE_CHARS + 1], footer[48];
        const size_t first = page * 2;
        memcpy(line1, text + start[first], length[first]);
        line1[length[first]] = '\0';
        line2[0] = '\0';
        if (first + 1 < lines) {
            memcpy(line2, text + start[first + 1], length[first + 1]);
            line2[length[first + 1]] = '\0';
        }
        (void)snprintf(footer, sizeof(footer), "%u/%u   %s", (unsigned)(page + 1), (unsigned)pages, NAV_FOOTER);
        screen_text(title, line1, line2, footer);
        switch (wait_key()) {
        case KEY_SELECT:
            return true;
        case KEY_NEXT:
            if (page + 1 >= pages) {
                return true;
            }
            ++page;
            break;
        case KEY_PREV:
            if (!page) {
                return false;
            }
            --page;
            break;
        case KEY_REDRAW:
            break;
        default:
            return false;
        }
    }
}

static void show_qr(const char* value)
{
    /* Repaint the code if the timeout warning covered it, then leave on any key. */
    while (seedtool_display_qr(value) && wait_key() == KEY_REDRAW) {
    }
}

static size_t layout_keys(const char* layout)
{
    size_t count = 0;
    for (const char* cursor = layout; *cursor; ++cursor) {
        if (*cursor != '\n') {
            ++count;
        }
    }
    return count;
}

static char layout_key(const char* layout, size_t index)
{
    for (const char* cursor = layout; *cursor; ++cursor) {
        if (*cursor != '\n' && !index--) {
            return *cursor;
        }
    }
    return '\0';
}

static size_t step_key(const bool* enabled, const size_t count, size_t index, const bool forward)
{
    for (size_t i = 0; i < count; ++i) {
        index = forward ? (index + 1) % count : (index + count - 1) % count;
        if (!enabled || enabled[index]) {
            return index;
        }
    }
    return index;
}

static size_t first_enabled_key(const bool* enabled, const size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (enabled[i]) {
            return i;
        }
    }
    return 0;
}

/* One BIP39 word. Returns 1 when a word was chosen, 0 when the user deleted
 * back out of this word, and -1 on timeout or overflow. Only letters that can
 * still lead to a word are reachable, and once ten or fewer words match the
 * remaining candidates are offered directly. The initial key and the candidate
 * order are deterministic; nothing here consults the RNG. */
static int enter_word(const size_t position, const size_t total, char* output, const size_t output_len)
{
    char stem[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
    size_t stem_len = 0;
    char title[24];
    (void)snprintf(title, sizeof(title), "Word %u of %u", (unsigned)position, (unsigned)total);

    for (;;) {
        uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
        const size_t matches = seedtool_words_with_prefix(stem, stem_len, candidates, SEEDTOOL_MAX_WORD_CHOICES);
        bool erase = false;

        if (!matches) {
            seedtool_zero(stem, sizeof(stem));
            return -1;
        }
        if (matches <= SEEDTOOL_MAX_WORD_CHOICES) {
            /* Small candidate set: pick the word itself, or the delete entry. */
            size_t selected = 0;
            bool picked = false;
            while (!picked) {
                char footer[48];
                (void)snprintf(footer, sizeof(footer), "%u/%u   %s", (unsigned)(selected + 1),
                    (unsigned)(matches + 1), NAV_FOOTER);
                screen_text(title, selected == matches ? "[delete]" : seedtool_word(candidates[selected]), stem,
                    footer);
                switch (wait_key()) {
                case KEY_SELECT:
                    picked = true;
                    break;
                case KEY_PREV:
                    selected = (selected + matches) % (matches + 1);
                    break;
                case KEY_NEXT:
                    selected = (selected + 1) % (matches + 1);
                    break;
                case KEY_REDRAW:
                    break;
                default:
                    seedtool_zero(stem, sizeof(stem));
                    return -1;
                }
            }
            if (selected < matches) {
                const char* const word = seedtool_word(candidates[selected]);
                const int result = strlen(word) + 1 > output_len ? -1 : 1;
                if (result == 1) {
                    strcpy(output, word);
                }
                seedtool_zero(stem, sizeof(stem));
                return result;
            }
            erase = true;
        } else {
            /* Too many candidates to list: narrow the stem one letter at a time. */
            bool enabled[WORD_KEYS] = { false };
            seedtool_next_letters(stem, stem_len, enabled);
            enabled[WORD_KEYS - 1] = true;
            size_t selected = first_enabled_key(enabled, WORD_KEYS);
            bool picked = false;
            while (!picked) {
                seedtool_display_keyboard(title, stem_len ? stem : "-", WORD_LAYOUT, enabled, selected);
                switch (wait_key()) {
                case KEY_SELECT:
                    picked = true;
                    break;
                case KEY_PREV:
                    selected = step_key(enabled, WORD_KEYS, selected, false);
                    break;
                case KEY_NEXT:
                    selected = step_key(enabled, WORD_KEYS, selected, true);
                    break;
                case KEY_REDRAW:
                    break;
                default:
                    seedtool_zero(stem, sizeof(stem));
                    return -1;
                }
            }
            if (selected == WORD_KEYS - 1) {
                erase = true;
            } else if (stem_len < SEEDTOOL_MAX_WORD_LEN) {
                stem[stem_len++] = (char)('a' + selected);
                stem[stem_len] = '\0';
            }
        }

        if (erase) {
            if (!stem_len) {
                seedtool_zero(stem, sizeof(stem));
                return 0;
            }
            stem[--stem_len] = '\0';
        }
    }
}

static bool enter_mnemonic(const size_t count, char* mnemonic, const size_t mnemonic_len)
{
    char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
    bool ok = true;
    size_t index = 0;
    while (index < count) {
        const int result = enter_word(index + 1, count, words[index], sizeof(words[index]));
        if (result < 0) {
            ok = false;
            break;
        }
        if (result == 0) {
            /* Deleting past the start of a word steps back to the previous one,
             * and past the first word abandons entry entirely. */
            if (!index) {
                ok = false;
                break;
            }
            words[--index][0] = '\0';
            continue;
        }
        ++index;
    }

    size_t used = 0;
    for (size_t i = 0; ok && i < count; ++i) {
        const size_t n = strlen(words[i]);
        if (used + n + (i ? 1 : 0) + 1 > mnemonic_len) {
            ok = false;
            break;
        }
        if (i) {
            mnemonic[used++] = ' ';
        }
        memcpy(mnemonic + used, words[i], n + 1);
        used += n;
    }
    seedtool_zero(words, sizeof(words));
    return ok;
}

static bool enter_passphrase_once(char* output, const size_t output_len)
{
    size_t used = 0, page = 0, selected = 0;
    output[0] = '\0';
    for (;;) {
        const char* const layout = passphrase_layouts[page];
        const size_t keys = layout_keys(layout);
        if (selected >= keys) {
            selected = 0;
        }
        const char* const tail = used > PASSPHRASE_TAIL ? output + used - PASSPHRASE_TAIL : output;
        seedtool_display_keyboard("BIP39 passphrase", tail, layout, NULL, selected);
        switch (wait_key()) {
        case KEY_PREV:
            selected = (selected + keys - 1) % keys;
            continue;
        case KEY_NEXT:
            selected = (selected + 1) % keys;
            continue;
        case KEY_SELECT:
            break;
        case KEY_REDRAW:
            continue;
        default:
            return false;
        }
        const char pressed = layout_key(layout, selected);
        if (pressed == SEEDTOOL_KEY_ACCEPT) {
            return true;
        }
        if (pressed == SEEDTOOL_KEY_PAGE) {
            page = (page + 1) % PASSPHRASE_PAGES;
            selected = 0;
        } else if (pressed == SEEDTOOL_KEY_BACKSPACE) {
            if (used) {
                output[--used] = '\0';
            }
        } else if (used + 1 < output_len) {
            output[used++] = pressed;
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
    uint8_t fp[4] = { 0 };
    char fphex[9] = { 0 };
    char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1] = { 0 };
    char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
    char address[SEEDTOOL_MAX_ADDRESS_LEN] = { 0 };
    uint32_t index = 0;

    if (!get_session_passphrase(passphrase)) {
        goto done;
    }
    if (seedtool_master_fingerprint(mnemonic, passphrase, fp) != SEEDTOOL_OK) {
        (void)acknowledge("Error", "Derivation failed", NULL);
        goto done;
    }
    hexstr(fp, sizeof(fp), fphex);

    for (;;) {
        char position[32];
        (void)snprintf(position, sizeof(position), "Address index: %u", (unsigned)index);
        const char* menu[] = { "Master fingerprint", "Account xpub BIP84", "Account xpub BIP86", "Address BIP84 bc1q",
            "Address BIP86 bc1p", position, "Done / erase" };
        const int selected = choose("Wallet", menu, sizeof(menu) / sizeof(menu[0]));
        if (selected < 0 || selected == 6) {
            break;
        }
        if (selected == 0) {
            (void)acknowledge(
                "Master fingerprint", fphex, passphrase[0] ? "Passphrase: session only" : "Passphrase: none");
        } else if (selected == 1 || selected == 2) {
            const seedtool_address_type_t type = selected == 1 ? SEEDTOOL_BIP84 : SEEDTOOL_BIP86;
            char origin[32];
            (void)snprintf(origin, sizeof(origin), "[%s/%u'/0'/0']", fphex, (unsigned)type);
            if (seedtool_account_xpub(mnemonic, passphrase, type, xpub, sizeof(xpub)) == SEEDTOOL_OK) {
                (void)page_text(origin, xpub);
            } else {
                (void)acknowledge("Error", "Could not derive xpub", NULL);
            }
            seedtool_zero(xpub, sizeof(xpub));
        } else if (selected == 3 || selected == 4) {
            const seedtool_address_type_t type = selected == 3 ? SEEDTOOL_BIP84 : SEEDTOOL_BIP86;
            char path[32];
            (void)snprintf(path, sizeof(path), "m/%u'/0'/0'/0/%u", (unsigned)type, (unsigned)index);
            if (seedtool_mainnet_address(mnemonic, passphrase, type, index, address, sizeof(address))
                == SEEDTOOL_OK) {
                if (page_text(path, address)
                    && acknowledge("Address QR", "Raw address only", "No xpub / no metadata")) {
                    show_qr(address);
                }
            } else {
                (void)acknowledge("Error", "Could not derive address", NULL);
            }
            seedtool_zero(address, sizeof(address));
        } else {
            unsigned chosen = 0;
            if (enter_value("Address index", 0, 0, 0, 99, &chosen, NULL, NULL)) {
                index = chosen;
            }
        }
    }
done:
    seedtool_zero(fp, sizeof(fp));
    seedtool_zero(fphex, sizeof(fphex));
    seedtool_zero(xpub, sizeof(xpub));
    seedtool_zero(address, sizeof(address));
    seedtool_zero(passphrase, sizeof(passphrase));
}

static void show_generated(seedtool_generated_t* generated)
{
    char hash[65];
    hexstr(generated->hash, sizeof(generated->hash), hash);
    if (page_text("Canonical transcript", generated->transcript) && page_text("SHA256", hash)
        && page_text("BIP39 mnemonic", generated->mnemonic)) {
        show_wallet_data(generated->mnemonic);
    }
    seedtool_zero(hash, sizeof(hash));
}

static void create_seed(void)
{
    const char* sources[] = { "D6 dice", "D20 dice", "Coin flips", "Cards", "Back" };
    const int source = choose("Entropy source", sources, sizeof(sources) / sizeof(sources[0]));
    if (source < 0 || source == 4) {
        return;
    }
    const char* lengths[] = { "12 words", "24 words" };
    const int length = source == SEEDTOOL_CARDS ? 0 : choose("Seed length", lengths, 2);
    if (length < 0) {
        return;
    }
    const size_t words = length ? 24 : 12;
    const size_t required = seedtool_required_events((seedtool_source_t)source, words);
    const unsigned min = (source == SEEDTOOL_COIN || source == SEEDTOOL_CARDS) ? 0 : 1;
    const unsigned max = source == SEEDTOOL_D6 ? 6 : source == SEEDTOOL_D20 ? 20 : source == SEEDTOOL_COIN ? 1 : 51;
    const format_fn format = source == SEEDTOOL_CARDS ? format_card : source == SEEDTOOL_COIN ? format_coin : NULL;

    uint8_t values[256] = { 0 };
    bool available[52];
    seedtool_generated_t generated;
    bool complete = true;
    memset(available, 1, sizeof(available));
    memset(&generated, 0, sizeof(generated));

    for (size_t i = 0; i < required && complete; ++i) {
        unsigned value = 0;
        complete = enter_value(sources[source], (unsigned)(i + 1), (unsigned)required, min, max, &value, format,
            source == SEEDTOOL_CARDS ? available : NULL);
        if (complete) {
            values[i] = (uint8_t)value;
            if (source == SEEDTOOL_CARDS) {
                available[value] = false;
            }
        }
    }
    if (complete) {
        if (seedtool_generate((seedtool_source_t)source, words, values, required, &generated) == SEEDTOOL_OK) {
            show_generated(&generated);
        } else {
            (void)acknowledge("Error", "Could not generate seed", NULL);
        }
    }
    seedtool_zero(values, sizeof(values));
    seedtool_zero(available, sizeof(available));
    seedtool_zero(&generated, sizeof(generated));
}

static void complete_checksum(void)
{
    const char* lengths[] = { "11 words + 7 coins", "23 words + 3 coins", "Back" };
    const int selected = choose("Complete checksum", lengths, 3);
    if (selected < 0 || selected == 2) {
        return;
    }
    const size_t count = selected ? 23 : 11;
    const size_t bits_count = selected ? 3 : 7;
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    uint8_t bits[7] = { 0 };
    bool ok = enter_mnemonic(count, prefix, sizeof(prefix));
    for (size_t i = 0; ok && i < bits_count; ++i) {
        unsigned bit = 0;
        ok = enter_value("Coin flip", (unsigned)(i + 1), (unsigned)bits_count, 0, 1, &bit, format_coin, NULL);
        bits[i] = (uint8_t)bit;
    }
    if (ok && seedtool_complete_checksum(prefix, bits, bits_count, completed, sizeof(completed)) == SEEDTOOL_OK
        && page_text("Completed mnemonic", completed)) {
        show_wallet_data(completed);
    }
    seedtool_zero(prefix, sizeof(prefix));
    seedtool_zero(completed, sizeof(completed));
    seedtool_zero(bits, sizeof(bits));
}

static void restore_seed(void)
{
    const char* lengths[] = { "12 words", "24 words", "Back" };
    const int selected = choose("Restore mnemonic", lengths, 3);
    if (selected < 0 || selected == 2) {
        return;
    }
    char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    if (enter_mnemonic(selected ? 24 : 12, mnemonic, sizeof(mnemonic))) {
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
    last_action = seedtool_platform_milliseconds();

    /* The opening screen deliberately ignores the navigation buttons. It is
     * where the user is still learning that both buttons together mean select,
     * so a stray press must not be read as "go back" and restart the device. */
    for (seedtool_key_t key = KEY_PREV; key != KEY_SELECT;) {
        screen_text("ORIGO", "OFFLINE / STATELESS", "User entropy only", "BOTH buttons to continue");
        key = wait_key();
        if (key == KEY_TIMEOUT) {
            seedtool_platform_restart();
        }
    }
    for (;;) {
        const char* menu[] = { "Create Seed", "Restore Seed", "Complete Checksum", "About / Safety", "Reboot" };
        const int selected = choose("Origo", menu, sizeof(menu) / sizeof(menu[0]));
        if (selected < 0 || selected == 4) {
            seedtool_platform_restart();
        } else if (selected == 0) {
            create_seed();
        } else if (selected == 1) {
            restore_seed();
        } else if (selected == 2) {
            complete_checksum();
        } else {
            (void)page_text("Safety",
                "No seed is stored. No radio, wallet signing, PIN, OTA or serial RPC. Verify the firmware hash and "
                "record entropy independently. Left and right move, both buttons together select.");
        }
        last_action = seedtool_platform_milliseconds();
    }
}
