#include "seedtool_core.h"

#include <stdio.h>
#include <string.h>
#include <wally_core.h>

#include "seedtool_app.h"
#include "seedtool_display.h"
#include "seedtool_platform.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#define SESSION_TIMEOUT_MS (10 * 60 * 1000)
#define WARNING_TIMEOUT_MS (60 * 1000)
#define SPLASH_MS 2500
#define MAX_PAGE_LINES 24
#define MAX_LINE_CHARS 48
#define PASSPHRASE_TAIL 24

#define NAV_FOOTER "L/R move   BOTH select"
#define ACK_FOOTER "BOTH continue   L/R back"

/* Word entry keyboard: the letters plus backspace. */
#define WORD_LAYOUT SEEDTOOL_WORD_LAYOUT
#define WORD_KEYS (SEEDTOOL_LETTERS + 1)

/* Word number keyboard, for a backup that records numbers rather than words. */
#define WORD_NUMBER_LAYOUT SEEDTOOL_WORD_NUMBER_LAYOUT
#define WORD_NUMBER_KEYS (SEEDTOOL_DIGITS + 2)

/* Every key is found by its character rather than by where it sits, so the
 * layouts are QWERTY, or anything else, purely by changing the strings above. */
#define PASSPHRASE_PAGES SEEDTOOL_PASSPHRASE_PAGES
const char* const seedtool_passphrase_layouts[PASSPHRASE_PAGES] = {
    "qwertyuiop\nasdfghjkl \nzxcvbnm\b\t\r",
    "QWERTYUIOP\nASDFGHJKL \nZXCVBNM\b\t\r",
    "1234567890\n!\"#$%&'()\n*+,-./ \b\t\r",
    ":;<=>?@\n[\\]^_`~\n{|} \b\t\r",
};

typedef void (*format_fn)(unsigned value, char* output, size_t output_len);

static uint64_t last_action;

/* Pressing a button and watching what happens teaches left, right and hold. It
 * cannot teach the chord, because nothing moves until both are released. So the
 * hint stays until the chord has been used once, and then never comes back:
 * every screen after that is the counter alone. */
static bool chord_learned;

static const char* nav_hint(void) { return chord_learned ? "" : "   " NAV_FOOTER; }

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
    if (key == KEY_SELECT) {
        chord_learned = true;
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

/* A screen the reader has to leave deliberately. Returns the key that left it,
 * so a caller that must tell "went back" from "timed out" can. */
static seedtool_key_t confirm(const char* title, const char* one, const char* two)
{
    for (;;) {
        screen_text(title, one, two, ACK_FOOTER);
        const seedtool_key_t key = wait_key();
        if (key != KEY_REDRAW) {
            return key;
        }
    }
}

static bool acknowledge(const char* title, const char* one, const char* two)
{
    return confirm(title, one, two) == KEY_SELECT;
}

/* Every choice in the firmware is made here, on a list that shows five options
 * at once. Which rows are on screen follows from the count and the selection
 * alone, so a choice screen is reproducible from what the user has done. */
static int choose(const char* title, const char* const* items, const size_t count)
{
    size_t selected = 0, top = 0;
    for (;;) {
        char footer[48];
        (void)snprintf(footer, sizeof(footer), "%u/%u%s", (unsigned)(selected + 1), (unsigned)count, nav_hint());
        top = seedtool_list_top(count, selected, top);
        seedtool_display_list(title, items, count, selected, top, footer);
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
 * A `total` of zero means this is a one-off value rather than one of a run.
 *
 * Returns 1 when a value was chosen, 0 when the user stepped onto `[back]`, and
 * -1 on timeout — the same contract as enter_word(). Left and right are spending
 * their two gestures on the value itself, so backing out has to be a position in
 * the ring rather than a key, exactly as `[delete]` is in the word list. Without
 * it a mistake on roll 29 of 50 could only be escaped by waiting out the session
 * timeout and starting the whole transcript again. */
static int enter_value(const char* title, const unsigned position, const unsigned total, const unsigned min,
    const unsigned max, unsigned* value, const format_fn format, const bool* allowed, const char* history)
{
    const unsigned first = allowed && !allowed[0] ? step_value(min, min, max, true, allowed) : min;
    /* One step back from the first value wraps to the last reachable one. */
    const unsigned last = step_value(first, min, max, false, allowed);
    unsigned current = first;
    bool on_back = false;

    for (;;) {
        char heading[48], shown[48];
        if (total) {
            (void)snprintf(heading, sizeof(heading), "%s  %u/%u", title, position, total);
        } else {
            (void)snprintf(heading, sizeof(heading), "%s  %u-%u", title, min, max);
        }
        if (format) {
            format(current, shown, sizeof(shown));
        } else {
            (void)snprintf(shown, sizeof(shown), "%u", current);
        }
        /* The running transcript sits under the value being entered, so a
         * mis-keyed roll is caught against the paper now rather than at the end
         * of ninety-nine of them. Only its tail fits, which is the part that
         * just changed. */
        screen_text(heading, on_back ? "[back]" : shown, history, chord_learned ? NULL : NAV_FOOTER);
        switch (wait_key()) {
        case KEY_SELECT:
            if (on_back) {
                return 0;
            }
            *value = current;
            return 1;
        case KEY_PREV:
            if (on_back) {
                on_back = false;
                current = last;
            } else if (current == first) {
                on_back = true;
            } else {
                current = step_value(current, min, max, false, allowed);
            }
            break;
        case KEY_NEXT:
            if (on_back) {
                on_back = false;
                current = first;
            } else if (current == last) {
                on_back = true;
            } else {
                current = step_value(current, min, max, true, allowed);
            }
            break;
        case KEY_REDRAW:
            break;
        default:
            return -1;
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
        (void)snprintf(footer, sizeof(footer), "%u/%u%s", (unsigned)(page + 1), (unsigned)pages, nav_hint());
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

/* One exportable value. The account keys carry their key origin, so a scan does
 * not have to be told the derivation path afterwards. */
typedef struct {
    char title[24];
    char value[sizeof("[00000000/84'/0'/0']") + SEEDTOOL_MAX_XPUB_LEN];
} qr_item_t;

#define QR_ITEMS 4

/* The QR screen steps sideways through everything the wallet can be given, so an
 * account key and the address it belongs to are one press apart. */
static void show_qr(const qr_item_t* items, const size_t count, size_t selected)
{
    while (count) {
        if (!seedtool_display_qr(items[selected].title, items[selected].value)) {
            (void)acknowledge("Too long for a QR", items[selected].title, "Read it as text instead");
            return;
        }
        switch (wait_key()) {
        case KEY_PREV:
            selected = (selected + count - 1) % count;
            break;
        case KEY_NEXT:
            selected = (selected + 1) % count;
            break;
        case KEY_REDRAW:
            break;
        default:
            return;
        }
    }
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

/* The enabled key closest to `index`, searching outwards and looking ahead
 * before behind. The cursor keeps its place between letters, so it has to land
 * somewhere sensible when the letter it was on stops leading to a word. */
static size_t nearest_enabled(const bool* enabled, const size_t count, const size_t index)
{
    if (!count || enabled[index % count]) {
        return count ? index % count : 0;
    }
    for (size_t distance = 1; distance <= count / 2 + 1; ++distance) {
        const size_t ahead = (index + distance) % count;
        if (enabled[ahead]) {
            return ahead;
        }
        const size_t behind = (index + count - distance) % count;
        if (enabled[behind]) {
            return behind;
        }
    }
    return index % count;
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
    /* The cursor opens at the centre and then keeps its place between letters,
     * so a word is not retyped from the far corner every time. */
    size_t selected = seedtool_layout_center(WORD_LAYOUT);
    char title[24];
    (void)snprintf(title, sizeof(title), "Word %u/%u", (unsigned)position, (unsigned)total);

    for (;;) {
        uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
        const size_t matches = seedtool_words_with_prefix(stem, stem_len, candidates, SEEDTOOL_MAX_WORD_CHOICES);
        bool erase = false;

        if (!matches) {
            seedtool_zero(stem, sizeof(stem));
            return -1;
        }
        if (matches <= SEEDTOOL_MAX_WORD_CHOICES) {
            /* Small candidate set: the remaining words are listed outright, with
             * the delete entry last. The list has taken over the area the stem
             * used to occupy, so the stem rides in the title instead. */
            const char* items[SEEDTOOL_MAX_WORD_CHOICES + 1];
            char listing[sizeof(title) + SEEDTOOL_MAX_WORD_LEN + 3];
            for (size_t i = 0; i < matches; ++i) {
                items[i] = seedtool_word(candidates[i]);
            }
            items[matches] = "[delete]";
            (void)snprintf(listing, sizeof(listing), "%s  %s", title, stem_len ? stem : "-");
            const int chosen = choose(listing, items, matches + 1);
            seedtool_zero(listing, sizeof(listing));
            if (chosen < 0) {
                seedtool_zero(stem, sizeof(stem));
                return -1;
            }
            if ((size_t)chosen < matches) {
                const char* const word = items[chosen];
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
            bool letters[SEEDTOOL_LETTERS] = { false };
            bool enabled[WORD_KEYS] = { false };
            (void)seedtool_next_letters(stem, stem_len, letters);
            for (size_t i = 0; i < WORD_KEYS; ++i) {
                const char key = seedtool_layout_key(WORD_LAYOUT, i);
                enabled[i] = key == SEEDTOOL_KEY_BACKSPACE || letters[key - 'a'];
            }
            selected = nearest_enabled(enabled, WORD_KEYS, selected);
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
            const char key = seedtool_layout_key(WORD_LAYOUT, selected);
            if (key == SEEDTOOL_KEY_BACKSPACE) {
                erase = true;
            } else if (stem_len < SEEDTOOL_MAX_WORD_LEN) {
                stem[stem_len++] = key;
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

/* One BIP39 word, entered as its one-based number instead of its letters, for a
 * backup that records numbers. Same contract as enter_word(): 1 when a word was
 * chosen, 0 when the user deleted back out of this word, -1 on timeout or
 * overflow. Only digits that still lead to a word number are reachable, and the
 * number is shown as its word before it is accepted: a digit misread off paper
 * is otherwise a different seed with no sign that anything went wrong. */
static int enter_word_number(const size_t position, const size_t total, char* output, const size_t output_len)
{
    char digits[SEEDTOOL_MAX_WORD_DIGITS + 1] = { 0 };
    size_t digits_len = 0;
    size_t selected = seedtool_layout_center(WORD_NUMBER_LAYOUT);
    char title[24];
    (void)snprintf(title, sizeof(title), "Word %u/%u", (unsigned)position, (unsigned)total);

    for (;;) {
        bool reachable[SEEDTOOL_DIGITS] = { false };
        bool enabled[WORD_NUMBER_KEYS] = { false };
        (void)seedtool_next_digits(digits, digits_len, reachable);
        const unsigned number = seedtool_word_number(digits, digits_len);
        for (size_t i = 0; i < WORD_NUMBER_KEYS; ++i) {
            const char key = seedtool_layout_key(WORD_NUMBER_LAYOUT, i);
            enabled[i] = key == SEEDTOOL_KEY_BACKSPACE ? true
                : key == SEEDTOOL_KEY_ACCEPT           ? number != 0
                                                       : reachable[key - '0'];
        }
        selected = nearest_enabled(enabled, WORD_NUMBER_KEYS, selected);
        bool picked = false;
        while (!picked) {
            seedtool_display_keyboard(title, digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected);
            switch (wait_key()) {
            case KEY_SELECT:
                picked = true;
                break;
            case KEY_PREV:
                selected = step_key(enabled, WORD_NUMBER_KEYS, selected, false);
                break;
            case KEY_NEXT:
                selected = step_key(enabled, WORD_NUMBER_KEYS, selected, true);
                break;
            case KEY_REDRAW:
                break;
            default:
                seedtool_zero(digits, sizeof(digits));
                return -1;
            }
        }

        const char pressed = seedtool_layout_key(WORD_NUMBER_LAYOUT, selected);
        if (pressed == SEEDTOOL_KEY_ACCEPT) {
            const char* const word = seedtool_word(number - 1);
            char counted[32];
            (void)snprintf(counted, sizeof(counted), "Number %u of %u", number, (unsigned)SEEDTOOL_WORDLIST_LEN);
            const seedtool_key_t key = confirm(title, word, counted);
            seedtool_zero(counted, sizeof(counted));
            if (key == KEY_SELECT) {
                const int result = !word || strlen(word) + 1 > output_len ? -1 : 1;
                if (result == 1) {
                    strcpy(output, word);
                }
                seedtool_zero(digits, sizeof(digits));
                return result;
            }
            if (key == KEY_TIMEOUT) {
                seedtool_zero(digits, sizeof(digits));
                return -1;
            }
            /* Went back: the number is cleared rather than left half-entered, so
             * what is on screen is always the whole of what was typed. */
            seedtool_zero(digits, sizeof(digits));
            digits_len = 0;
        } else if (pressed == SEEDTOOL_KEY_BACKSPACE) {
            if (!digits_len) {
                seedtool_zero(digits, sizeof(digits));
                return 0;
            }
            digits[--digits_len] = '\0';
        } else if (digits_len < SEEDTOOL_MAX_WORD_DIGITS) {
            digits[digits_len++] = pressed;
            digits[digits_len] = '\0';
        }
    }
}

/* Returns 1 when a whole mnemonic was entered, 0 when the user backed out of the
 * first word or the method menu, -1 on timeout or overflow. */
static int enter_mnemonic(const size_t count, char* mnemonic, const size_t mnemonic_len)
{
    const char* methods[] = { "Type the letters", "Enter word numbers", "Back" };
    const int method = choose("Word entry", methods, 3);
    if (method < 0) {
        return -1;
    }
    if (method == 2) {
        return 0;
    }
    char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
    int outcome = 1;
    size_t index = 0;
    while (index < count) {
        const int result = method ? enter_word_number(index + 1, count, words[index], sizeof(words[index]))
                                  : enter_word(index + 1, count, words[index], sizeof(words[index]));
        if (result < 0) {
            outcome = -1;
            break;
        }
        if (result == 0) {
            /* Deleting past the start of a word steps back to the previous one,
             * and past the first word returns to the method menu. */
            if (!index) {
                outcome = 0;
                break;
            }
            words[--index][0] = '\0';
            continue;
        }
        ++index;
    }

    size_t used = 0;
    for (size_t i = 0; outcome == 1 && i < count; ++i) {
        const size_t n = strlen(words[i]);
        if (used + n + (i ? 1 : 0) + 1 > mnemonic_len) {
            outcome = -1;
            break;
        }
        if (i) {
            mnemonic[used++] = ' ';
        }
        memcpy(mnemonic + used, words[i], n + 1);
        used += n;
    }
    seedtool_zero(words, sizeof(words));
    return outcome;
}

static bool enter_passphrase_once(char* output, const size_t output_len)
{
    size_t used = 0, page = 0;
    size_t selected = seedtool_layout_center(seedtool_passphrase_layouts[0]);
    output[0] = '\0';
    for (;;) {
        const char* const layout = seedtool_passphrase_layouts[page];
        const size_t keys = seedtool_layout_keys(layout);
        if (selected >= keys) {
            selected = seedtool_layout_center(layout);
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
        const char pressed = seedtool_layout_key(layout, selected);
        if (pressed == SEEDTOOL_KEY_ACCEPT) {
            return true;
        }
        if (pressed == SEEDTOOL_KEY_PAGE) {
            page = (page + 1) % PASSPHRASE_PAGES;
            /* A new page is a new keyboard, so the cursor starts from its centre
             * rather than from wherever the page key happened to sit. */
            selected = seedtool_layout_center(seedtool_passphrase_layouts[page]);
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
    const char* options[] = { "No passphrase", "Enter passphrase", "Back" };
    const int selected = choose("Optional passphrase", options, 3);
    if (selected != 1) {
        passphrase[0] = '\0';
        /* Only "No passphrase" derives; back and timeout both leave without. */
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

/* Derives all four exportable values up front so stepping between them is
 * instant rather than a fresh BIP32 derivation per press. */
static bool build_qr_items(const char* mnemonic, const char* passphrase, const char* fphex, const uint32_t index,
    qr_item_t items[QR_ITEMS])
{
    static const seedtool_address_type_t types[] = { SEEDTOOL_BIP84, SEEDTOOL_BIP86 };
    bool ok = true;
    for (size_t i = 0; ok && i < 2; ++i) {
        char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
        ok = seedtool_account_xpub(mnemonic, passphrase, types[i], xpub, sizeof(xpub)) == SEEDTOOL_OK;
        if (ok) {
            (void)snprintf(items[i].title, sizeof(items[i].title), "BIP%u account key", (unsigned)types[i]);
            (void)snprintf(
                items[i].value, sizeof(items[i].value), "[%s/%u'/0'/0']%s", fphex, (unsigned)types[i], xpub);
        }
        seedtool_zero(xpub, sizeof(xpub));
    }
    for (size_t i = 0; ok && i < 2; ++i) {
        char address[SEEDTOOL_MAX_ADDRESS_LEN] = { 0 };
        ok = seedtool_mainnet_address(mnemonic, passphrase, types[i], index, address, sizeof(address)) == SEEDTOOL_OK;
        if (ok) {
            (void)snprintf(items[2 + i].title, sizeof(items[2 + i].title), "BIP%u address %u", (unsigned)types[i],
                (unsigned)index);
            (void)snprintf(items[2 + i].value, sizeof(items[2 + i].value), "%s", address);
        }
        seedtool_zero(address, sizeof(address));
    }
    return ok;
}

/* Entering the QR screen from anywhere reaches the account keys, so the warning
 * is about them rather than about the value the reader started from. */
static void export_qr(const char* mnemonic, const char* passphrase, const char* fphex, const uint32_t index,
    const size_t selected)
{
    qr_item_t items[QR_ITEMS];
    memset(items, 0, sizeof(items));
    if (!acknowledge("QR export", "Account keys included", "A photo reveals every address")) {
        seedtool_zero(items, sizeof(items));
        return;
    }
    if (build_qr_items(mnemonic, passphrase, fphex, index, items)) {
        show_qr(items, QR_ITEMS, selected);
    } else {
        (void)acknowledge("Error", "Could not derive", NULL);
    }
    seedtool_zero(items, sizeof(items));
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
                if (page_text(origin, xpub)) {
                    export_qr(mnemonic, passphrase, fphex, index, selected == 1 ? 0 : 1);
                }
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
                if (page_text(path, address)) {
                    export_qr(mnemonic, passphrase, fphex, index, selected == 3 ? 2 : 3);
                }
            } else {
                (void)acknowledge("Error", "Could not derive address", NULL);
            }
            seedtool_zero(address, sizeof(address));
        } else {
            unsigned chosen = 0;
            if (enter_value("Address index", 0, 0, 0, 99, &chosen, NULL, NULL, NULL) == 1) {
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

/* Collects the whole transcript and generates from it. Returns 1 when a seed was
 * produced, 0 when the user backed out of the first entry, -1 on timeout. */
static int collect_entropy(const int source, const size_t words)
{
    static const char* const names[] = { "D6 dice", "D20 dice", "Coin flips", "Cards" };
    const size_t required = seedtool_required_events((seedtool_source_t)source, words);
    const unsigned min = (source == SEEDTOOL_COIN || source == SEEDTOOL_CARDS) ? 0 : 1;
    const unsigned max = source == SEEDTOOL_D6 ? 6 : source == SEEDTOOL_D20 ? 20 : source == SEEDTOOL_COIN ? 1 : 51;
    const format_fn format = source == SEEDTOOL_CARDS ? format_card : source == SEEDTOOL_COIN ? format_coin : NULL;

    uint8_t values[256] = { 0 };
    bool available[52];
    char history[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1] = { 0 };
    seedtool_generated_t generated;
    int outcome = 1;
    size_t i = 0;
    memset(available, 1, sizeof(available));
    memset(&generated, 0, sizeof(generated));

    while (i < required) {
        unsigned value = 0;
        /* Rebuilt from the values each time rather than appended to, so what is
         * on screen is the transcript itself and cannot drift from it — going
         * back a step has to unwrite the last entry too. */
        if (seedtool_transcript((seedtool_source_t)source, values, i, history, sizeof(history)) != SEEDTOOL_OK) {
            history[0] = '\0';
        }
        const size_t shown = strlen(history) - seedtool_render_fit_tail(history);
        const int result = enter_value(names[source], (unsigned)(i + 1), (unsigned)required, min, max, &value, format,
            source == SEEDTOOL_CARDS ? available : NULL, history + shown);
        if (result < 0) {
            outcome = -1;
            break;
        }
        if (result == 0) {
            if (!i) {
                outcome = 0;
                break;
            }
            --i;
            /* A card put back becomes drawable again, or correcting an entry
             * would leave it permanently out of the deck. */
            if (source == SEEDTOOL_CARDS) {
                available[values[i]] = true;
            }
            continue;
        }
        values[i] = (uint8_t)value;
        if (source == SEEDTOOL_CARDS) {
            available[value] = false;
        }
        ++i;
    }
    if (outcome == 1) {
        if (seedtool_generate((seedtool_source_t)source, words, values, required, &generated) == SEEDTOOL_OK) {
            show_generated(&generated);
        } else {
            (void)acknowledge("Error", "Could not generate seed", NULL);
        }
    }
    seedtool_zero(values, sizeof(values));
    seedtool_zero(available, sizeof(available));
    seedtool_zero(history, sizeof(history));
    seedtool_zero(&generated, sizeof(generated));
    return outcome;
}

static void create_seed(void)
{
    for (;;) {
        const char* sources[] = { "D6 dice", "D20 dice", "Coin flips", "Cards", "Back" };
        const int source = choose("Entropy source", sources, sizeof(sources) / sizeof(sources[0]));
        if (source < 0 || source == 4) {
            return;
        }
        if (source == SEEDTOOL_CARDS) {
            /* Cards are 12 words only, so there is no length to choose and
             * backing out of the first card returns to the source menu. */
            if (collect_entropy(source, 12) != 0) {
                return;
            }
            continue;
        }
        for (;;) {
            const char* lengths[] = { "12 words", "24 words", "Back" };
            const int length = choose("Seed length", lengths, sizeof(lengths) / sizeof(lengths[0]));
            if (length < 0) {
                return;
            }
            if (length == 2) {
                break;
            }
            if (collect_entropy(source, length ? 24 : 12) != 0) {
                return;
            }
        }
    }
}

static void complete_checksum(void)
{
    for (;;) {
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
        int outcome = enter_mnemonic(count, prefix, sizeof(prefix));
        for (size_t i = 0; outcome == 1 && i < bits_count;) {
            unsigned bit = 0;
            char flips[8] = { 0 };
            for (size_t f = 0; f < i; ++f) {
                flips[f] = (char)('0' + bits[f]);
            }
            const int result = enter_value("Coin flip", (unsigned)(i + 1), (unsigned)bits_count, 0, 1, &bit,
                format_coin, NULL, flips);
            if (result < 0) {
                outcome = -1;
            } else if (result == 0) {
                /* Backing out of the first flip returns to the words. */
                if (!i) {
                    outcome = enter_mnemonic(count, prefix, sizeof(prefix));
                } else {
                    --i;
                }
            } else {
                bits[i] = (uint8_t)bit;
                ++i;
            }
        }
        if (outcome == 1 && seedtool_complete_checksum(prefix, bits, bits_count, completed, sizeof(completed))
                == SEEDTOOL_OK
            && page_text("Completed mnemonic", completed)) {
            show_wallet_data(completed);
        }
        seedtool_zero(prefix, sizeof(prefix));
        seedtool_zero(completed, sizeof(completed));
        seedtool_zero(bits, sizeof(bits));
        if (outcome != 0) {
            return;
        }
    }
}

static void restore_seed(void)
{
    for (;;) {
        const char* lengths[] = { "12 words", "24 words", "Back" };
        const int selected = choose("Restore mnemonic", lengths, 3);
        if (selected < 0 || selected == 2) {
            return;
        }
        char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        const int outcome = enter_mnemonic(selected ? 24 : 12, mnemonic, sizeof(mnemonic));
        if (outcome == 1) {
            if (seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK) {
                if (acknowledge("Checksum valid", "BIP39 English", "Derivation unlocked")) {
                    show_wallet_data(mnemonic);
                }
            } else {
                (void)acknowledge("INVALID CHECKSUM", "Addresses are blocked", "Check every word");
            }
        }
        seedtool_zero(mnemonic, sizeof(mnemonic));
        if (outcome != 0) {
            return;
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

    /* The logo holds the screen for a moment and the menu then takes over on its
     * own. Every press is discarded meanwhile: this is where the user is still
     * learning that both buttons together mean select, and a screen that offers
     * no choice must not turn a stray press into one. */
    seedtool_display_splash();
    for (const uint64_t deadline = seedtool_platform_milliseconds() + SPLASH_MS;;) {
        const uint64_t now = seedtool_platform_milliseconds();
        if (now >= deadline) {
            break;
        }
        (void)seedtool_platform_wait_key((uint32_t)(deadline - now));
    }
    last_action = seedtool_platform_milliseconds();

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
