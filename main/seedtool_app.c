#include "seedtool_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

#include "seedtool_app.h"
#include "seedtool_bbqr.h"
#include "seedtool_display.h"
#include "seedtool_platform.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#define SESSION_TIMEOUT_MS (10 * 60 * 1000)
#define WARNING_TIMEOUT_MS (60 * 1000)
/* Bits of slack below the minimum before a run is flagged, on top of the
 * Miller-Madow correction (seedtool_dice_entropy_bias_bits). Krux's
 * ENTROPY_TOLERANCE. */
#define DICE_ENTROPY_TOLERANCE 4
#define SPLASH_MS 2500
#define MAX_PAGE_LINES 24
#define MAX_LINE_CHARS 48
#define PASSPHRASE_TAIL 24

/* Every index up to SEEDTOOL_MAX_ADDRESS_INDEX is derived and cached; only the
 * first ADDRESS_SHOWN_ROWS get a row, and "Go to index" reaches the rest. */
#define ADDRESS_LIST_ROWS (SEEDTOOL_MAX_ADDRESS_INDEX + 1)
#define ADDRESS_SHOWN_ROWS 50
#define ADDRESS_LABEL_LEN (8 + SEEDTOOL_MAX_ADDRESS_LEN)
/* derive_addresses labels one shown row per derived address, so showing more
 * rows than were derived would read past the end of `addresses`. */
_Static_assert(ADDRESS_SHOWN_ROWS <= ADDRESS_LIST_ROWS, "more address rows are shown than are derived");

/* The two buttons sit one above the other, so the left one (KEY_PREV) reads as
 * "up". Spelled out because the fonts have no arrow glyph. */
#define NAV_FOOTER "Up/Down move   BOTH select"
/* The digit field keeps a footer: up and down change the digit, so the chrome's
 * two controls have nothing to attach to. */
#define DIGIT_FOOTER "Up/Down set   BOTH take"

/* Word entry keyboard: the letters plus backspace. */
#define WORD_LAYOUT SEEDTOOL_WORD_LAYOUT
#define WORD_KEYS (SEEDTOOL_LETTERS + 1)

/* Word number keyboard, for a backup that records numbers rather than words. */
#define WORD_NUMBER_LAYOUT SEEDTOOL_WORD_NUMBER_LAYOUT
#define WORD_NUMBER_KEYS (SEEDTOOL_DIGITS + 2)

/* The account index uses the word-number keypad. Three digits reach
 * SEEDTOOL_MAX_ACCOUNT_INDEX, so every digit is reachable at any length. */
#define ACCOUNT_DIGITS 3

/* "Go to index", sized for SEEDTOOL_MAX_ADDRESS_INDEX. The assertion keeps the
 * two in step, and browse_addresses also rejects an index past the cache. */
#define ADDRESS_INDEX_DIGITS 2
_Static_assert(ADDRESS_INDEX_DIGITS == 2 && SEEDTOOL_MAX_ADDRESS_INDEX == 99,
    "the address-index keypad width and the derived address range must be changed together");

/* The word-number field draws one box per digit, and the renderer bounds how
 * many of those fit across the display. Widening a word number without the
 * boxes to show it would run the row off both edges, which clips silently. */
_Static_assert(SEEDTOOL_MAX_WORD_DIGITS <= SEEDTOOL_DIGIT_BOXES_MAX,
    "a word number has more digits than the digit field can draw boxes for");

#define PASSPHRASE_PAGES SEEDTOOL_PASSPHRASE_PAGES

typedef void (*format_fn)(unsigned value, char* output, size_t output_len);

static uint64_t last_action;

/* The chord hint shows until the chord has been used once. */
static bool chord_learned;

/* Display settings live only in RAM and reset every boot. */
static bool orientation_flipped;
static unsigned backlight_level = SEEDTOOL_DISPLAY_BRIGHTNESS_DEFAULT;

static void seedtool_require(const bool condition)
{
    if (!condition) {
        seedtool_platform_restart();
    }
}

/* Shows what a seedtool_render_qr* call drew. Nothing is drawn, and nothing
 * shown, when the payload does not fit. */
static bool show_qr(const bool drawn)
{
    if (drawn) {
        seedtool_display_flush();
    }
    return drawn;
}

static void screen_text(const char* title, const char* line1, const char* line2, const char* footer)
{
    seedtool_render_screen(title, line1, line2, footer);
    seedtool_display_flush();
}

/* A flipped case turns the buttons round too, so PREV and NEXT are swapped
 * here, where every key passes. */
static seedtool_key_t wait_key_raw(const uint32_t timeout_ms)
{
    seedtool_key_t key = seedtool_platform_wait_key(timeout_ms);
    if (key != KEY_TIMEOUT) {
        last_action = seedtool_platform_milliseconds();
    }
    if (key == KEY_SELECT) {
        chord_learned = true;
    }
    if (orientation_flipped) {
        if (key == KEY_PREV) {
            key = KEY_NEXT;
        } else if (key == KEY_NEXT) {
            key = KEY_PREV;
        }
    }
    return key;
}

/* Anything but an extension ends the session by restarting, wherever it was:
 * unwinding back to the menu would repaint every screen on the way, secrets
 * included. The restart is the erase (see scrub_free_heap). An extended
 * session returns KEY_REDRAW, because the warning replaced the caller's
 * screen. */
static seedtool_key_t session_timeout(void)
{
    screen_text("Session timeout", "Secrets will be erased", "in 60 seconds", "BOTH extend   Up/Down erase");
    /* A press begun before this screen existed cannot be an answer to it. */
    seedtool_platform_flush_keys();
    if (wait_key_raw(WARNING_TIMEOUT_MS) != KEY_SELECT) {
        seedtool_platform_restart();
    }
    return KEY_REDRAW;
}

static uint32_t idle_ms(void)
{
    const uint64_t elapsed = seedtool_platform_milliseconds() - last_action;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

/* Never returns KEY_TIMEOUT. */
static seedtool_key_t wait_key(void)
{
    const uint32_t idle = idle_ms();
    if (idle < SESSION_TIMEOUT_MS) {
        const seedtool_key_t key = wait_key_raw(SESSION_TIMEOUT_MS - idle);
        if (key != KEY_TIMEOUT) {
            return key;
        }
    }
    return session_timeout();
}

/* wait_key for a screen that redraws on a timer: returns KEY_TIMEOUT, with
 * *ticked set, every frame_ms with no key down. */
static seedtool_key_t wait_key_or_tick(const uint32_t frame_ms, bool* const ticked)
{
    *ticked = false;
    const uint32_t idle = idle_ms();
    if (idle < SESSION_TIMEOUT_MS) {
        const uint32_t remaining = SESSION_TIMEOUT_MS - idle;
        const uint32_t wait_ms = frame_ms < remaining ? frame_ms : remaining;
        const seedtool_key_t key = wait_key_raw(wait_ms);
        if (key != KEY_TIMEOUT) {
            return key;
        }
        if (wait_ms == frame_ms) {
            *ticked = true;
            return KEY_TIMEOUT;
        }
    }
    return session_timeout();
}


/* choose_nav's outcomes other than an item index. */
#define NAV_TIMEOUT (-1)
#define NAV_BACK (-2)
#define NAV_CONFIRM (-3)

/* The ring the cursor walks: the arrow, the items, then the confirm, either end
 * absent when the screen does not offer it. */
static size_t nav_ring_size(const size_t count, const bool confirm_enabled, const bool back)
{
    return count + (back ? 1 : 0) + (confirm_enabled ? 1 : 0);
}

static size_t nav_position(const size_t selected, const size_t count, const bool back)
{
    if (selected == SEEDTOOL_NAV_BACK) {
        return 0;
    }
    const size_t offset = back ? 1 : 0;
    return selected == SEEDTOOL_NAV_CONFIRM ? count + offset : selected + offset;
}

static size_t nav_selection(const size_t position, const size_t count, const bool back)
{
    if (back && !position) {
        return SEEDTOOL_NAV_BACK;
    }
    const size_t index = back ? position - 1 : position;
    return index >= count ? SEEDTOOL_NAV_CONFIRM : index;
}

/* The loop behind every screen whose only controls are the arrow and the
 * confirm. `progress` adds the entropy bar; `back` false removes the arrow.
 * Returns NAV_BACK, NAV_CONFIRM or NAV_TIMEOUT. */
static int nav_screen(const char* title, const char* one, const char* two, const char* label, const bool back,
    const bool start_on_back, const seedtool_progress_t* progress)
{
    seedtool_nav_t nav = {
        /* A screen with no confirm opens on the arrow, whatever the caller
         * asked. */
        .selected = back && (start_on_back || !label) ? SEEDTOOL_NAV_BACK : SEEDTOOL_NAV_CONFIRM,
        .confirm = label ? SEEDTOOL_CONFIRM_TICK : SEEDTOOL_CONFIRM_NONE,
        .confirm_enabled = true,
        .back = back,
    };
    for (;;) {
        if (progress) {
            seedtool_render_nav_dice(&nav, title, one, two, progress);
        } else {
            seedtool_render_nav_text(&nav, title, one, two, NULL);
        }
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            return nav.selected == SEEDTOOL_NAV_BACK ? NAV_BACK : NAV_CONFIRM;
        case KEY_PREV:
        case KEY_NEXT:
            /* With both controls either key moves to the other; with one there
             * is nowhere to move. */
            if (back && label) {
                nav.selected = nav.selected == SEEDTOOL_NAV_BACK ? SEEDTOOL_NAV_CONFIRM : SEEDTOOL_NAV_BACK;
            }
            break;
        case KEY_REDRAW:
            break;
        default:
            return NAV_TIMEOUT;
        }
    }
}

/* A screen the reader takes or leaves. `start_on_back` is the opt-out from
 * opening on the bar, for a screen whose whole purpose is to say "this may be
 * a bad idea". */
static bool nav_acknowledge(
    const char* title, const char* one, const char* two, const char* label, const bool start_on_back)
{
    return nav_screen(title, one, two, label, true, start_on_back, NULL) == NAV_CONFIRM;
}

/* The same with the entropy quality bar drawn in. */
static bool nav_dice_confirm(const char* title, const char* one, const char* two, const char* label,
    const bool start_on_back, const seedtool_progress_t* progress)
{
    return nav_screen(title, one, two, label, true, start_on_back, progress) == NAV_CONFIRM;
}

/* A notice: the reader reads it and leaves, so the arrow is its only control. */
static void notice(const char* title, const char* one, const char* two)
{
    (void)nav_screen(title, one, two, NULL, true, true, NULL);
}

/* A list under the nav chrome. `cursor` carries the selection in and out, so a
 * screen reopened after an edit lands where it was; seed it with
 * SEEDTOOL_NAV_CONFIRM, SEEDTOOL_NAV_BACK or an item index.
 *
 * `back` is false only for the Origo menu, which has no level above it, and the
 * wallet, whose exit erases the session and so is a row rather than a one-press
 * arrow. */
static int choose_nav(const char* title, const char* const* items, const size_t count, const char* confirm,
    const bool confirm_enabled, const bool back, size_t* const cursor)
{
    size_t top = 0;
    /* A cursor on a control this draw does not offer falls back to the arrow,
     * or to the first item where there is no arrow. */
    if ((*cursor == SEEDTOOL_NAV_CONFIRM && !confirm_enabled)
        || (*cursor == SEEDTOOL_NAV_BACK && !back)
        || (*cursor != SEEDTOOL_NAV_CONFIRM && *cursor != SEEDTOOL_NAV_BACK && *cursor >= count)) {
        *cursor = back ? SEEDTOOL_NAV_BACK : 0;
    }
    for (;;) {
        if (*cursor != SEEDTOOL_NAV_BACK && *cursor != SEEDTOOL_NAV_CONFIRM) {
            top = seedtool_list_top(count, *cursor, top);
        }
        const seedtool_nav_t nav = {
            .selected = *cursor,
            .confirm = confirm ? SEEDTOOL_CONFIRM_TICK : SEEDTOOL_CONFIRM_NONE,
            .confirm_enabled = confirm_enabled,
            .back = back,
        };
        seedtool_render_nav_list(&nav, title, items, count, top);
        seedtool_display_flush();
        const size_t ring = nav_ring_size(count, confirm_enabled, back);
        size_t position = nav_position(*cursor, count, back);
        switch (wait_key()) {
        case KEY_SELECT:
            if (*cursor == SEEDTOOL_NAV_BACK) {
                return NAV_BACK;
            }
            return *cursor == SEEDTOOL_NAV_CONFIRM ? NAV_CONFIRM : (int)*cursor;
        case KEY_PREV:
            *cursor = nav_selection((position + ring - 1) % ring, count, back);
            break;
        case KEY_NEXT:
            *cursor = nav_selection((position + 1) % ring, count, back);
            break;
        case KEY_REDRAW:
            break;
        default:
            return NAV_TIMEOUT;
        }
    }
}

/* A paged screen's cursor: 0 is the arrow, 1..pages the pages and pages + 1 the
 * confirm, so moving down reads forward. Without `confirmable` the ring is the
 * arrow and the pages. */
static size_t page_step(const size_t position, const size_t pages, const bool forward, const bool confirmable)
{
    const size_t ring = pages + (confirmable ? 2 : 1);
    return (position + (forward ? 1 : ring - 1)) % ring;
}

/* The page the body shows for a cursor position: the arrow reads with the
 * first page behind it, the bar with the last. */
static size_t page_shown(const size_t position, const size_t pages)
{
    return !position ? 0 : position > pages ? pages - 1 : position - 1;
}

static size_t page_selection(const size_t position, const size_t pages)
{
    if (!position) {
        return SEEDTOOL_NAV_BACK;
    }
    return position > pages ? SEEDTOOL_NAV_CONFIRM : SEEDTOOL_NAV_BODY;
}

/* The chrome a paged screen wears. Paged text hands off to another screen
 * (FORWARD); a numbered list is agreed to (TICK). */
static seedtool_nav_t page_nav(const size_t position, const size_t pages, const char* counter,
    const bool confirmable, const seedtool_confirm_t style)
{
    const seedtool_nav_t nav = {
        .selected = page_selection(position, pages),
        /* Reading screens have nothing to agree to, so no control. */
        .confirm = confirmable ? style : SEEDTOOL_CONFIRM_NONE,
        .confirm_enabled = true,
        .back = true,
        .counter = counter,
    };
    return nav;
}

/* A menu: its rows are its actions, so it has no confirm. The arrow returns
 * `count`, one past the last row. */
static int choose_menu_at(const char* title, const char* const* items, const size_t count, size_t* const cursor)
{
    const int selected = choose_nav(title, items, count, NULL, false, true, cursor);
    if (selected == NAV_BACK) {
        return (int)count;
    }
    return selected == NAV_TIMEOUT ? -1 : selected;
}

static int choose_menu(const char* title, const char* const* items, const size_t count)
{
    size_t cursor = 0;
    return choose_menu_at(title, items, count, &cursor);
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

/* The "N/M" counter an entry screen's title carries, plus the bits so far when
 * `progress` is graded. The minimum is shown elsewhere; leaving it out keeps a
 * coin's title clear of the edge. */
static void format_progress_heading(char* heading, const size_t heading_len, const char* title,
    const unsigned position, const unsigned total, const seedtool_progress_t* progress)
{
    if (progress && progress->graded) {
        (void)snprintf(heading, heading_len, "%s %u/%u %d bits", title, position, total, progress->bits);
    } else {
        (void)snprintf(heading, heading_len, "%s  %u/%u", title, position, total);
    }
}

/* Numeric carousel. `allowed` is optional and indexed from `min`; disallowed
 * values (cards already drawn) are skipped. A `total` of zero is a one-off
 * value rather than one of a run.
 *
 * Returns 1 with `*value` set, 0 when the reader took `[back]`, or -1 on
 * timeout. `[back]` is a stop on the ring because both keys change the value. */
static int enter_value(const char* title, const unsigned position, const unsigned total, const unsigned min,
    const unsigned max, unsigned* value, const format_fn format, const bool* allowed, const char* history,
    const seedtool_progress_t* progress)
{
    const unsigned first = allowed && !allowed[0] ? step_value(min, min, max, true, allowed) : min;
    /* One step back from the first value wraps to the last reachable one. */
    const unsigned last = step_value(first, min, max, false, allowed);
    unsigned current = first;
    bool on_back = false;

    for (;;) {
        char heading[48], shown[48];
        if (total) {
            format_progress_heading(heading, sizeof(heading), title, position, total, progress);
        } else {
            (void)snprintf(heading, sizeof(heading), "%s  %u-%u", title, min, max);
        }
        if (format) {
            format(current, shown, sizeof(shown));
        } else {
            (void)snprintf(shown, sizeof(shown), "%u", current);
        }
        /* A plain number scrolls in its own box. A formatted value (a card) is
         * too wide for one and uses the text screen, with the running
         * transcript under it so a mis-keyed entry is caught against the paper. */
        if (!format) {
            seedtool_render_value_box(heading, shown, on_back, chord_learned ? NULL : NAV_FOOTER, progress);
        } else if (progress) {
            seedtool_render_dice_screen(
                heading, on_back ? "[back]" : shown, history, chord_learned ? NULL : NAV_FOOTER, progress);
        } else {
            seedtool_render_screen(heading, on_back ? "[back]" : shown, history, chord_learned ? NULL : NAV_FOOTER);
        }
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            if (on_back) {
                return 0;
            }
            *value = current;
            return 1;
        /* KEY_PREV, the physical "up" button, raises the value, as on a
         * spinner. */
        case KEY_PREV:
            if (on_back) {
                on_back = false;
                current = first;
            } else if (current == last) {
                on_back = true;
            } else {
                current = step_value(current, min, max, true, allowed);
            }
            break;
        case KEY_NEXT:
            if (on_back) {
                on_back = false;
                current = last;
            } else if (current == first) {
                on_back = true;
            } else {
                current = step_value(current, min, max, false, allowed);
            }
            break;
        case KEY_REDRAW:
            break;
        default:
            return -1;
        }
    }
}

/* One coin flip: KEY_PREV ("up") is heads, KEY_NEXT tails, and the chord undoes
 * the last flip. One press per flip, for runs of 128 or 256. Returns 1 with
 * `*bit` set, 0 to step back, -1 on timeout. */
static int enter_coin_flip(const char* title, const unsigned position, const unsigned total, unsigned* bit,
    const char* history, const seedtool_progress_t* progress)
{
    char heading[48];
    format_progress_heading(heading, sizeof(heading), title, position, total, progress);
    for (;;) {
        seedtool_render_dice_screen(heading, "Heads (up)   Tails (down)", history, NULL, progress);
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            return 0;
        case KEY_PREV:
            *bit = 1;
            return 1;
        case KEY_NEXT:
            *bit = 0;
            return 1;
        case KEY_REDRAW:
            break;
        default:
            return -1;
        }
    }
}

static const char CARD_RANKS[] = "A23456789TJQK";
static const char* const CARD_SUIT_NAMES[4] = { "Clubs", "Diamonds", "Hearts", "Spades" };

static void format_rank(const unsigned value, char* output, const size_t output_len)
{
    (void)snprintf(output, output_len, "%c", CARD_RANKS[value]);
}

static void format_suit(const unsigned value, char* output, const size_t output_len)
{
    (void)snprintf(output, output_len, "%s", CARD_SUIT_NAMES[value]);
}

/* A card in two carousels, suit then rank: about 8.5 presses a card rather than
 * 26 through one 52-way ring. Backing out of the rank returns to the suit;
 * backing out of the suit undoes the card. Same contract as enter_value. */
/* `available` NULL means every card can be drawn (with replacement). */
static int enter_card(const unsigned position, const unsigned total, unsigned* value, const bool* available,
    const char* history, const seedtool_progress_t* progress)
{
    for (;;) {
        bool suit_allowed[4];
        for (unsigned s = 0; s < 4; ++s) {
            suit_allowed[s] = !available;
            for (unsigned r = 0; available && r < 13; ++r) {
                if (available[s * 13 + r]) {
                    suit_allowed[s] = true;
                    break;
                }
            }
        }
        unsigned suit = 0;
        const int suit_result
            = enter_value("Suit", position, total, 0, 3, &suit, format_suit, suit_allowed, history, progress);
        if (suit_result <= 0) {
            return suit_result;
        }

        bool rank_allowed[13];
        for (unsigned r = 0; r < 13; ++r) {
            rank_allowed[r] = !available || available[suit * 13 + r];
        }
        unsigned rank = 0;
        const int rank_result = enter_value(
            CARD_SUIT_NAMES[suit], position, total, 0, 12, &rank, format_rank, rank_allowed, history, progress);
        if (rank_result < 0) {
            return -1;
        }
        if (rank_result == 0) {
            continue;
        }
        *value = suit * 13 + rank;
        return 1;
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

/* Paged text, three lines a page, split by what fits on the display. Returns
 * true when the reader advanced past the last page or accepted, false when they
 * backed out. */
static bool page_text_impl(const char* title, const char* text, const bool confirmable, const bool grouped)
{
    size_t start[MAX_PAGE_LINES], length[MAX_PAGE_LINES];
    const size_t total = strlen(text);
    size_t lines = 0, offset = 0;
    while (lines < MAX_PAGE_LINES && (offset < total || !lines)) {
        const size_t fit = grouped ? seedtool_render_fit_grouped(text + offset, MAX_LINE_CHARS)
                                   : seedtool_render_fit(text + offset, MAX_LINE_CHARS);
        start[lines] = offset;
        length[lines] = fit;
        /* A glyph wider than the display would otherwise loop forever. */
        offset += fit ? fit : 1;
        ++lines;
    }
    const size_t pages = (lines + 2) / 3;
    /* Opens on the first page, not on the confirm bar: reading is what this
     * screen is for, so the cursor starts where the reading does. */
    size_t position = 1;
    bool advanced;
    /* Outside the loop so the one exit can wipe them: they hold whatever is
     * paged, the mnemonic included. */
    char line1[MAX_LINE_CHARS + 1], line2[MAX_LINE_CHARS + 1], line3[MAX_LINE_CHARS + 1];
    for (;;) {
        char footer[48];
        const size_t page = page_shown(position, pages);
        const size_t first = page * 3;
        memcpy(line1, text + start[first], length[first]);
        line1[length[first]] = '\0';
        line2[0] = '\0';
        line3[0] = '\0';
        if (first + 1 < lines) {
            memcpy(line2, text + start[first + 1], length[first + 1]);
            line2[length[first + 1]] = '\0';
        }
        if (first + 2 < lines) {
            memcpy(line3, text + start[first + 2], length[first + 2]);
            line3[length[first + 2]] = '\0';
        }
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
        const seedtool_nav_t nav = page_nav(position, pages, footer, confirmable, SEEDTOOL_CONFIRM_FORWARD);
        if (grouped) {
            /* Each line's position in groups, so the alternating ink carries
             * across line breaks. */
            const char* const shown[] = { line1, line2, line3 };
            const size_t first_group[] = { start[first] / SEEDTOOL_GROUP_LEN,
                (first + 1 < lines ? start[first + 1] : 0) / SEEDTOOL_GROUP_LEN,
                (first + 2 < lines ? start[first + 2] : 0) / SEEDTOOL_GROUP_LEN };
            seedtool_render_nav_grouped(&nav, title, shown, first_group, 3);
        } else {
            seedtool_render_nav_text(&nav, title, line1, line2, line3);
        }
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            if (!position) {
                advanced = false;
                goto done;
            }
            if (position > pages) {
                advanced = true;
                goto done;
            }
            /* The chord on a page reads on. */
            position = page_step(position, pages, true, confirmable);
            break;
        case KEY_NEXT:
            position = page_step(position, pages, true, confirmable);
            break;
        case KEY_PREV:
            position = page_step(position, pages, false, confirmable);
            break;
        case KEY_REDRAW:
            break;
        default:
            advanced = false;
            goto done;
        }
    }
done:
    seedtool_zero(line1, sizeof(line1));
    seedtool_zero(line2, sizeof(line2));
    seedtool_zero(line3, sizeof(line3));
    return advanced;
}

/* Paged text with something to take at the end of it - the descriptor, the
 * account key, the transcript and its hash, the completed mnemonic. The
 * caller acts on the answer, so the tick is there to give one. */
static bool page_text(const char* title, const char* text)
{
    return page_text_impl(title, text, true, false);
}


/* Paged text with nothing to take: read, then leave by the arrow. */
static void page_read(const char* title, const char* text)
{
    (void)page_text_impl(title, text, false, false);
}

/* The account key carries its key origin, so a scan does not have to be told
 * the derivation path afterwards. The account component's "999" is
 * SEEDTOOL_MAX_ACCOUNT_INDEX's own width, not a placeholder. */
#define ACCOUNT_KEY_LEN (sizeof("[00000000/84'/0'/999']") + SEEDTOOL_MAX_XPUB_LEN)

/* A BIP380 descriptor: prefix, origin, xpub, BIP389's "<0;1>" multipath step,
 * the wildcard, "#" and 8 checksum characters. "wpkh" is the longest prefix
 * used. */
#define DESCRIPTOR_LEN (sizeof("wpkh([00000000/84'/0'/999']/<0;1>/*)#12345678") + SEEDTOOL_MAX_XPUB_LEN)

/* The longest value ever handed to show_bbqr: the descriptor. */
#define BBQR_MAX_VALUE_LEN ((ACCOUNT_KEY_LEN > DESCRIPTOR_LEN ? ACCOUNT_KEY_LEN : DESCRIPTOR_LEN) - 1)
#define BBQR_FRAME_LEN (SEEDTOOL_BBQR_HEADER_LEN + (BBQR_MAX_VALUE_LEN * 8 + 4) / 5 + 1)

/* Frames advance on a timer only; up and down move between the screen's two
 * controls, as on every QR. A reader collects parts across repeats, so a frame
 * the camera misses comes round again. */
#define BBQR_FRAME_INTERVAL_MS 1000

/* Frames are capped at version 3: 4px modules on this 135px display, where
 * versions 4-6 give 3px and 1-2 double the frame count. */
#define BBQR_FRAME_MAX_VERSION 3

/* An account key or descriptor as animated BBQr parts
 * (github.com/coinkite/BBQr), the wire format Krux reads (src/krux/bbqr.py).
 * Always the plain base32 "2" encoding: there is no deflate here. Returns the
 * key that ended the animation. */
static seedtool_key_t show_bbqr(const char* title, const char* value)
{
    const size_t len = strlen(value);
    const size_t frame_chars = seedtool_render_qr_alphanumeric_capacity(BBQR_FRAME_MAX_VERSION);
    const size_t parts = seedtool_bbqr_part_count(len, frame_chars);
    if (!parts) {
        notice("Too long for a QR", title, "Read it as text instead");
        return KEY_SELECT;
    }
    size_t part = 0;
    size_t selected = SEEDTOOL_NAV_BACK;
    for (;;) {
        char frame[BBQR_FRAME_LEN];
        /* Sized past any title plus "N/N", so -Wformat-truncation can prove it
         * fits. */
        char frame_title[48];
        if (parts > 1) {
            (void)snprintf(
                frame_title, sizeof(frame_title), "%.20s %u/%u", title, (unsigned)(part + 1), (unsigned)parts);
        } else {
            (void)snprintf(frame_title, sizeof(frame_title), "%.20s", title);
        }
        const bool ok = seedtool_bbqr_part((const uint8_t*)value, len, '2', 'U', part, parts, frame, sizeof(frame))
            && show_qr(seedtool_render_qr(frame_title, frame, selected));
        seedtool_zero(frame, sizeof(frame));
        if (!ok) {
            notice("Too long for a QR", title, "Read it as text instead");
            return KEY_SELECT;
        }
        /* A single part has nothing to advance to, so it waits like the still
         * screen it is rather than redrawing the same code every second. */
        bool ticked = false;
        const seedtool_key_t key
            = parts > 1 ? wait_key_or_tick(BBQR_FRAME_INTERVAL_MS, &ticked) : wait_key();
        if (ticked) {
            part = (part + 1) % parts;
            continue;
        }
        if (key == KEY_REDRAW) {
            continue;
        }
        if (key == KEY_PREV || key == KEY_NEXT) {
            selected = selected == SEEDTOOL_NAV_BACK ? SEEDTOOL_NAV_SHADE : SEEDTOOL_NAV_BACK;
            continue;
        }
        if (key == KEY_SELECT && selected == SEEDTOOL_NAV_SHADE) {
            seedtool_render_qr_cycle_shade();
            continue;
        }
        return key;
    }
}

/* An account key's QR, always through show_bbqr: it is too long for a legible
 * single frame. */
static void show_account_key_qr(const char* title, const char* value)
{
    for (;;) {
        const seedtool_key_t key = show_bbqr(title, value);
        if (key != KEY_REDRAW) {
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

/* Position of `target` in `layout`, or `count` if it is not one of its keys. */
static size_t layout_key_index(const char* layout, const char target)
{
    const size_t count = seedtool_layout_keys(layout);
    for (size_t i = 0; i < count; ++i) {
        if (seedtool_layout_key(layout, i) == target) {
            return i;
        }
    }
    return count;
}

/* One BIP39 word. Returns 1 when a word was chosen, 0 when the reader deleted
 * back out of it, -1 on timeout or overflow. Only letters that still lead to a
 * word are enabled, and ten or fewer candidates are listed outright.
 *
 * `allowed` narrows the words, NULL for all of them. Every enabled letter leads
 * to an allowed word, so the stem never runs out of candidates. */
static int enter_word(
    const size_t position, const size_t total, const seedtool_wordset_t* allowed, char* output, const size_t output_len)
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
        const size_t matches
            = seedtool_words_with_prefix_in(allowed, stem, stem_len, candidates, SEEDTOOL_MAX_WORD_CHOICES);
        bool erase = false;

        if (!matches) {
            seedtool_zero(stem, sizeof(stem));
            seedtool_zero(candidates, sizeof(candidates));
            return -1;
        }
        if (matches <= SEEDTOOL_MAX_WORD_CHOICES) {
            /* Ten or fewer candidates: list them, with the stem in the title. */
            const char* items[SEEDTOOL_MAX_WORD_CHOICES + 1];
            char listing[sizeof(title) + SEEDTOOL_MAX_WORD_LEN + 3];
            for (size_t i = 0; i < matches; ++i) {
                items[i] = seedtool_word(candidates[i]);
            }
            /* The arrow deletes a letter, or leaves the word from an empty
             * stem. It returns `matches`, one past the last row. */
            (void)snprintf(listing, sizeof(listing), "%s  %s", title, stem_len ? stem : "-");
            const int chosen = choose_menu(listing, items, matches);
            seedtool_zero(listing, sizeof(listing));
            if (chosen < 0) {
                seedtool_zero(stem, sizeof(stem));
                seedtool_zero(candidates, sizeof(candidates));
                return -1;
            }
            if ((size_t)chosen < matches) {
                const char* const word = items[chosen];
                const int result = strlen(word) + 1 > output_len ? -1 : 1;
                if (result == 1) {
                    strcpy(output, word);
                }
                seedtool_zero(stem, sizeof(stem));
                seedtool_zero(candidates, sizeof(candidates));
                return result;
            }
            erase = true;
        } else {
            /* Too many candidates to list: narrow the stem one letter at a time. */
            bool letters[SEEDTOOL_LETTERS] = { false };
            bool enabled[WORD_KEYS] = { false };
            (void)seedtool_next_letters_in(allowed, stem, stem_len, letters);
            for (size_t i = 0; i < WORD_KEYS; ++i) {
                const char key = seedtool_layout_key(WORD_LAYOUT, i);
                enabled[i] = key == SEEDTOOL_KEY_BACKSPACE || letters[key - 'a'];
            }
            selected = nearest_enabled(enabled, WORD_KEYS, selected);
            bool picked = false;
            while (!picked) {
                seedtool_render_keyboard(title, stem_len ? stem : "-", WORD_LAYOUT, enabled, selected, position, total);
                seedtool_display_flush();
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
                    seedtool_zero(candidates, sizeof(candidates));
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
                seedtool_zero(candidates, sizeof(candidates));
                return 0;
            }
            stem[--stem_len] = '\0';
        }
    }
}

/* One word by its one-based number (`abandon` = 1), for a backup that records
 * numbers. Same contract as enter_word. The number is shown as its word before
 * it is taken: a digit misread off paper would otherwise be a different seed
 * with no sign of it. `allowed` narrows it as the letter keyboard does.
 *
 * Up to four boxes, scrolled one at a time. Each box's ring holds the digits
 * that still lead to a word number, then OK once the digits already name one,
 * then backspace, so a number ends where the reader says: 240 is three digits,
 * not 0240. */
static int enter_word_number(
    const size_t position, const size_t total, const seedtool_wordset_t* allowed, char* output, const size_t output_len)
{
    char digits[SEEDTOOL_MAX_WORD_DIGITS + 1] = { 0 };
    /* What each box shows, including the one being scrolled. */
    char shown[SEEDTOOL_MAX_WORD_DIGITS + 1] = { 0 };
    size_t at = 0;
    char title[24];
    (void)snprintf(title, sizeof(title), "Word %u/%u", (unsigned)position, (unsigned)total);

    /* The ring for the current box, rebuilt when the box changes. */
    char ring[SEEDTOOL_DIGITS + 2];
    size_t ring_len = 0, on = 0;
    bool rebuild = true;

    for (;;) {
        if (rebuild) {
            bool reachable[SEEDTOOL_DIGITS] = { false };
            /* Narrowed by `allowed`, as the letter keyboard is. */
            (void)seedtool_next_digits_in(allowed, digits, at, reachable);
            ring_len = 0;
            for (size_t d = 0; d < SEEDTOOL_DIGITS; ++d) {
                if (reachable[d]) {
                    ring[ring_len++] = (char)('0' + d);
                }
            }
            /* OK once the digits name a word: `20` is both 20 and a step
             * towards 200x, so the reader says which. */
            if (seedtool_word_number_in(allowed, digits, at)) {
                ring[ring_len++] = SEEDTOOL_KEY_ACCEPT;
            }
            ring[ring_len++] = SEEDTOOL_KEY_BACKSPACE;
            on = 0;
            rebuild = false;
        }

        memset(shown, 0, sizeof(shown));
        memcpy(shown, digits, at);
        shown[at] = ring[on];
        for (size_t i = at + 1; i < SEEDTOOL_MAX_WORD_DIGITS; ++i) {
            shown[i] = ' ';
        }
        seedtool_render_digits(title, shown, SEEDTOOL_MAX_WORD_DIGITS, at, DIGIT_FOOTER, NULL);
        seedtool_display_flush();

        seedtool_key_t key = wait_key();
        if (key == KEY_REDRAW) {
            continue;
        }
        if (key == KEY_TIMEOUT) {
            seedtool_zero(digits, sizeof(digits));
            seedtool_zero(shown, sizeof(shown));
            return -1;
        }
        if (key == KEY_PREV) {
            on = on ? on - 1 : ring_len - 1;
            continue;
        }
        if (key == KEY_NEXT) {
            on = on + 1 < ring_len ? on + 1 : 0;
            continue;
        }

        /* KEY_SELECT: commit whatever the box is showing. */
        if (ring[on] == SEEDTOOL_KEY_BACKSPACE) {
            if (!at) {
                seedtool_zero(digits, sizeof(digits));
                seedtool_zero(shown, sizeof(shown));
                return 0;
            }
            digits[--at] = '\0';
            rebuild = true;
            continue;
        }
        bool typed_last = false;
        if (ring[on] != SEEDTOOL_KEY_ACCEPT) {
            digits[at++] = ring[on];
            digits[at] = '\0';
            rebuild = true;
            /* A fourth digit has nowhere left to go, so it confirms itself
             * rather than asking for an OK the field has no box to show. */
            if (at < SEEDTOOL_MAX_WORD_DIGITS) {
                continue;
            }
            typed_last = true;
        }

        /* Checked again here, not only where the ring is built: a number
         * outside the set sends the reader back to the field. */
        const unsigned number = seedtool_word_number_in(allowed, digits, at);
        const char* const word = number ? seedtool_word(number - 1) : NULL;
        char counted[32];
        (void)snprintf(counted, sizeof(counted), "Number %u of %u", number, (unsigned)SEEDTOOL_WORDLIST_LEN);
        /* The number just typed is what is being checked, so the bar names
         * it rather than saying "Continue". */
        const int answer = word ? nav_screen(title, word, counted, "Use this word", true, false, NULL) : NAV_BACK;
        seedtool_zero(counted, sizeof(counted));
        if (answer == NAV_CONFIRM) {
            const int result = strlen(word) + 1 > output_len ? -1 : 1;
            if (result == 1) {
                strcpy(output, word);
            }
            seedtool_zero(digits, sizeof(digits));
            seedtool_zero(shown, sizeof(shown));
            return result;
        }
        if (answer == NAV_TIMEOUT) {
            seedtool_zero(digits, sizeof(digits));
            seedtool_zero(shown, sizeof(shown));
            return -1;
        }
        /* Back from the confirmation: a number completed by its fourth digit
         * reopens that digit; one ended with OK returns to the field as it was. */
        if (typed_last) {
            digits[--at] = '\0';
        }
        rebuild = true;
    }
}

/* The account index, on the word-number keypad. Every digit is reachable up to
 * ACCOUNT_DIGITS, so nothing is pruned. Returns 1 with `*value` set, 0 when the
 * reader backed out of the first digit, -1 on timeout. */
static int enter_account(uint32_t* value)
{
    char digits[ACCOUNT_DIGITS + 1] = { 0 };
    size_t digits_len = 0;
    size_t selected = seedtool_layout_center(WORD_NUMBER_LAYOUT);
    const size_t accept_index = layout_key_index(WORD_NUMBER_LAYOUT, SEEDTOOL_KEY_ACCEPT);

    for (;;) {
        bool enabled[WORD_NUMBER_KEYS] = { false };
        for (size_t i = 0; i < WORD_NUMBER_KEYS; ++i) {
            const char key = seedtool_layout_key(WORD_NUMBER_LAYOUT, i);
            enabled[i] = key == SEEDTOOL_KEY_BACKSPACE ? true
                : key == SEEDTOOL_KEY_ACCEPT           ? digits_len > 0
                                                       : digits_len < ACCOUNT_DIGITS;
        }
        /* With the digits full, the cursor goes to Accept. */
        selected
            = !enabled[selected] && digits_len ? accept_index : nearest_enabled(enabled, WORD_NUMBER_KEYS, selected);
        bool picked = false;
        while (!picked) {
            seedtool_render_keyboard(
                "Account", digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected, 0, 0);
            seedtool_display_flush();
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
            uint32_t number = 0;
            for (size_t i = 0; i < digits_len; ++i) {
                number = number * 10 + (uint32_t)(digits[i] - '0');
            }
            seedtool_zero(digits, sizeof(digits));
            *value = number;
            return 1;
        }
        if (pressed == SEEDTOOL_KEY_BACKSPACE) {
            if (!digits_len) {
                seedtool_zero(digits, sizeof(digits));
                return 0;
            }
            digits[--digits_len] = '\0';
        } else if (digits_len < ACCOUNT_DIGITS) {
            digits[digits_len++] = pressed;
            digits[digits_len] = '\0';
        }
    }
}

/* enter_account for "Go to index", ADDRESS_INDEX_DIGITS wide. */
static int enter_address_index(uint32_t* value)
{
    char digits[ADDRESS_INDEX_DIGITS + 1] = { 0 };
    size_t digits_len = 0;
    size_t selected = seedtool_layout_center(WORD_NUMBER_LAYOUT);
    const size_t accept_index = layout_key_index(WORD_NUMBER_LAYOUT, SEEDTOOL_KEY_ACCEPT);

    for (;;) {
        bool enabled[WORD_NUMBER_KEYS] = { false };
        for (size_t i = 0; i < WORD_NUMBER_KEYS; ++i) {
            const char key = seedtool_layout_key(WORD_NUMBER_LAYOUT, i);
            enabled[i] = key == SEEDTOOL_KEY_BACKSPACE ? true
                : key == SEEDTOOL_KEY_ACCEPT           ? digits_len > 0
                                                       : digits_len < ADDRESS_INDEX_DIGITS;
        }
        selected = !enabled[selected] && digits_len ? accept_index
                                                     : nearest_enabled(enabled, WORD_NUMBER_KEYS, selected);
        bool picked = false;
        while (!picked) {
            seedtool_render_keyboard(
                "Go to index", digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected, 0, 0);
            seedtool_display_flush();
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
            uint32_t number = 0;
            for (size_t i = 0; i < digits_len; ++i) {
                number = number * 10 + (uint32_t)(digits[i] - '0');
            }
            seedtool_zero(digits, sizeof(digits));
            *value = number;
            return 1;
        }
        if (pressed == SEEDTOOL_KEY_BACKSPACE) {
            if (!digits_len) {
                seedtool_zero(digits, sizeof(digits));
                return 0;
            }
            digits[--digits_len] = '\0';
        } else if (digits_len < ADDRESS_INDEX_DIGITS) {
            digits[digits_len++] = pressed;
            digits[digits_len] = '\0';
        }
    }
}

/* words[0..count) joined with single spaces: the string
 * seedtool_validate_mnemonic sees. */
static bool join_words(
    char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, char* mnemonic, const size_t mnemonic_len)
{
    size_t used = 0;
    mnemonic[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        const size_t n = strlen(words[i]);
        if (used + n + (i ? 1 : 0) + 1 > mnemonic_len) {
            return false;
        }
        if (i) {
            mnemonic[used++] = ' ';
        }
        memcpy(mnemonic + used, words[i], n + 1);
        used += n;
    }
    return true;
}

/* Returns 1 when a whole mnemonic was entered, 0 when the reader backed out of
 * the first word or the method menu, -1 on timeout or overflow. `by_number`, if
 * not NULL, reports the entry method, which re-entering a word during review
 * reuses. */
static int enter_mnemonic_words(
    const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number)
{
    const char* methods[] = { "Type the letters", "Enter word numbers" };
    const int method = choose_menu("Word entry", methods, 2);
    if (method < 0) {
        return -1;
    }
    if (method == 2) {
        return 0;
    }
    if (by_number) {
        *by_number = method != 0;
    }
    int outcome = 1;
    size_t index = 0;
    seedtool_wordset_t allowed;
    while (index < count) {
        /* The last word of a whole mnemonic is narrowed to the words its
         * checksum allows, so a wrong one cannot be typed. Not for
         * complete_checksum's 11 or 23 words, whose last word carries no
         * checksum yet. */
        const bool last_of_whole = index + 1 == count && (count == 12 || count == 24);
        const seedtool_wordset_t* filter = NULL;
        if (last_of_whole) {
            char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
            if (!join_words(words, count - 1, prefix, sizeof(prefix))
                || seedtool_final_word_candidates(prefix, &allowed) != SEEDTOOL_OK) {
                seedtool_zero(prefix, sizeof(prefix));
                outcome = -1;
                break;
            }
            seedtool_zero(prefix, sizeof(prefix));
            filter = &allowed;
        }
        const int result = method
            ? enter_word_number(index + 1, count, filter, words[index], sizeof(words[index]))
            : enter_word(index + 1, count, filter, words[index], sizeof(words[index]));
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
    /* The set says something about the words before it. */
    seedtool_zero(&allowed, sizeof(allowed));
    return outcome;
}


static char review_labels[24][32];
static const char* review_items[24];

/* The entered words as a list, so any one can be fixed without retyping the
 * rest. Shown after every entry, not only a failed one. `mnemonic` is rejoined
 * after each edit, so the checksum is always graded on the current words.
 * Returns 1 on Continue with a valid checksum, 0 on Back, -1 on timeout or
 * overflow. */
static int review_and_confirm(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, const bool by_number,
    char* mnemonic, const size_t mnemonic_len)
{
    /* The confirm bar by default, as every nav screen opens: the reader who
     * has just typed twelve words is far more often done than not. */
    size_t cursor = SEEDTOOL_NAV_CONFIRM;
    int outcome;
    /* The rows are the mnemonic, in .bss: cleared on the way in, so a 12-word
     * restore shows none of an earlier 24-word one, and on the way out. */
    seedtool_zero(review_labels, sizeof(review_labels));
    for (;;) {
        if (!join_words(words, count, mnemonic, mnemonic_len)) {
            outcome = -1;
            goto done;
        }
        const bool valid = seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK;
        for (size_t i = 0; i < count; ++i) {
            /* %.*s: GCC's format-truncation analysis loses the bound on
             * words[i] through the parameter. */
            (void)snprintf(review_labels[i], sizeof(review_labels[i]), "%02u. %.*s", (unsigned)(i + 1),
                (int)SEEDTOOL_MAX_WORD_LEN, words[i]);
            review_items[i] = review_labels[i];
        }
        /* Continue stays drawn but dim while the checksum fails; the title
         * carries the verdict. */
        const int selected = choose_nav(
            valid ? "Review words" : "Review - fix a word", review_items, count, "Continue", valid, true, &cursor);
        if (selected == NAV_TIMEOUT) {
            outcome = -1;
            goto done;
        }
        if (selected == NAV_BACK) {
            outcome = 0;
            goto done;
        }
        if (selected == NAV_CONFIRM) {
            outcome = 1;
            goto done;
        }
        char word[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
        /* Re-entering the last word narrows it to what can end the current
         * words. If the set cannot be built the word is entered unnarrowed:
         * this screen must never block a fix. */
        seedtool_wordset_t allowed;
        const seedtool_wordset_t* filter = NULL;
        if ((size_t)selected + 1 == count) {
            char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
            if (join_words(words, count - 1, prefix, sizeof(prefix))
                && seedtool_final_word_candidates(prefix, &allowed) == SEEDTOOL_OK) {
                filter = &allowed;
            }
            seedtool_zero(prefix, sizeof(prefix));
        }
        const int result = by_number ? enter_word_number(selected + 1, count, filter, word, sizeof(word))
                                      : enter_word(selected + 1, count, filter, word, sizeof(word));
        seedtool_zero(&allowed, sizeof(allowed));
        if (result == 1) {
            strcpy(words[selected], word);
        } else if (result < 0) {
            seedtool_zero(word, sizeof(word));
            outcome = -1;
            goto done;
        }
        /* result == 0: backed out of re-entering this word, so it is left
         * exactly as it was and the list is simply shown again. */
        seedtool_zero(word, sizeof(word));
    }
done:
    seedtool_zero(review_labels, sizeof(review_labels));
    return outcome;
}

/* complete_checksum's 11 or 23 words, shown again when the reader steps back
 * from the flips: review_and_confirm without the checksum gate, which a prefix
 * can never pass. */
static int review_prefix(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, const bool by_number)
{
    size_t cursor = SEEDTOOL_NAV_CONFIRM;
    int outcome;
    seedtool_zero(review_labels, sizeof(review_labels));
    for (;;) {
        for (size_t i = 0; i < count; ++i) {
            (void)snprintf(review_labels[i], sizeof(review_labels[i]), "%02u. %.*s", (unsigned)(i + 1),
                (int)SEEDTOOL_MAX_WORD_LEN, words[i]);
            review_items[i] = review_labels[i];
        }
        const int selected = choose_nav("Review words", review_items, count, "Continue", true, true, &cursor);
        if (selected == NAV_TIMEOUT) {
            outcome = -1;
            goto done;
        }
        if (selected == NAV_BACK) {
            outcome = 0;
            goto done;
        }
        if (selected == NAV_CONFIRM) {
            outcome = 1;
            goto done;
        }
        char word[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
        /* No filter: none of these words carries the checksum. */
        const int result = by_number ? enter_word_number(selected + 1, count, NULL, word, sizeof(word))
                                     : enter_word(selected + 1, count, NULL, word, sizeof(word));
        if (result == 1) {
            strcpy(words[selected], word);
        } else if (result < 0) {
            seedtool_zero(word, sizeof(word));
            outcome = -1;
            goto done;
        }
        seedtool_zero(word, sizeof(word));
    }
done:
    seedtool_zero(review_labels, sizeof(review_labels));
    return outcome;
}

/* The words stage of a checksum completion: the list with the entry screen
 * behind it, so back steps one screen at a time. Returns 1 once the reader
 * comes forward, 0 when they step back past entry, -1 on timeout. */
static int revisit_prefix(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, bool* const by_number)
{
    for (;;) {
        const int reviewed = review_prefix(words, count, *by_number);
        if (reviewed != 0) {
            return reviewed;
        }
        const int entered = enter_mnemonic_words(count, words, by_number);
        if (entered != 1) {
            return entered;
        }
    }
}

/* Entry then review, for a mnemonic meant to be whole (complete_checksum's
 * partial one uses review_prefix). The last word is entered under its checksum
 * filter, so a completed entry always validates; the review still gates
 * Continue on it. */
static int restore_mnemonic(const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number,
    char* mnemonic, const size_t mnemonic_len, bool resume)
{
    /* The review's arrow reopens entry rather than the length menu. `resume`
     * starts on the review, for a caller handing the words back. */
    for (;;) {
        if (!resume) {
            const int entered = enter_mnemonic_words(count, words, by_number);
            if (entered != 1) {
                return entered;
            }
        }
        resume = false;
        const int reviewed = review_and_confirm(words, count, *by_number, mnemonic, mnemonic_len);
        if (reviewed != 0) {
            return reviewed;
        }
    }
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
        seedtool_render_keyboard("BIP39 passphrase", tail, layout, NULL, selected, 0, 0);
        seedtool_display_flush();
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

/* The optional passphrase, set from Derivation; a session starts without one.
 * Returns true only when it changed, so the caller re-derives the fingerprint.
 * Back, a timeout and a mismatch leave it as it was. */
static bool edit_session_passphrase(char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1])
{
    const char* options[] = { "No passphrase", "Enter passphrase" };
    const int selected = choose_menu("Passphrase", options, 2);
    if (selected < 0 || selected == 2) {
        return false;
    }
    if (selected == 0) {
        /* Already none: nothing changed, so nothing re-derives. */
        if (!passphrase[0]) {
            return false;
        }
        seedtool_zero(passphrase, SEEDTOOL_MAX_PASSPHRASE_LEN + 1);
        return true;
    }
    char attempt[SEEDTOOL_MAX_PASSPHRASE_LEN + 1] = { 0 };
    char confirmation[SEEDTOOL_MAX_PASSPHRASE_LEN + 1] = { 0 };
    const bool ok = enter_passphrase_once(attempt, sizeof(attempt))
        && nav_acknowledge(
               "Confirm passphrase", "Enter it a second time", "Exact match required", "Enter again", false)
        && enter_passphrase_once(confirmation, sizeof(confirmation)) && strcmp(attempt, confirmation) == 0;
    if (ok) {
        /* The old value goes before the new one lands, not after: this buffer
         * holds a live session secret and must never briefly hold a mix. */
        seedtool_zero(passphrase, SEEDTOOL_MAX_PASSPHRASE_LEN + 1);
        memcpy(passphrase, attempt, sizeof(attempt));
    } else {
        notice("Passphrase mismatch", "Passphrase unchanged", "Try again");
    }
    seedtool_zero(attempt, sizeof(attempt));
    seedtool_zero(confirmation, sizeof(confirmation));
    return ok;
}

/* The account key's own QR, warned about up front since a photo of it reveals
 * every address it can derive -- unlike a single address's QR below, which
 * reveals nothing more than itself. */
static void export_qr(const char* mnemonic, const char* passphrase, const char* fphex,
    const seedtool_address_type_t type, const uint32_t account, const seedtool_key_format_t format)
{
    /* Opens on the arrow: a photo of this reveals every address of the account. */
    if (!nav_acknowledge("QR export", "Account key included", "A photo reveals every address", "Show QR", true)) {
        return;
    }
    char title[24];
    (void)snprintf(title, sizeof(title), "BIP%u %s", (unsigned)type, format == SEEDTOOL_ZPUB ? "zpub" : "xpub");
    char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
    char value[ACCOUNT_KEY_LEN] = { 0 };
    if (seedtool_account_xpub(mnemonic, passphrase, type, account, format, xpub, sizeof(xpub)) == SEEDTOOL_OK) {
        (void)snprintf(value, sizeof(value), "[%s/%u'/0'/%u']%s", fphex, (unsigned)type, (unsigned)account, xpub);
        seedtool_zero(xpub, sizeof(xpub));
        show_account_key_qr(title, value);
    } else {
        seedtool_zero(xpub, sizeof(xpub));
        notice("Error", "Could not derive", NULL);
    }
    seedtool_zero(value, sizeof(value));
}

/* The account as a BIP380 descriptor, for a watch-only wallet to import. Always
 * xpub, since BIP380 has no SLIP-132. The chain step is BIP389's "<0;1>", so
 * one descriptor covers receive and change. */
static void show_descriptor(const char* mnemonic, const char* passphrase, const char* fphex,
    const seedtool_address_type_t type, const uint32_t account)
{
    /* On the arrow, for the reason the QR export above gives: it carries the
     * same account key and so the same warning. */
    if (!nav_acknowledge(
            "Descriptor export", "Account key included", "A photo reveals every address", "Show descriptor", true)) {
        return;
    }
    char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
    char body[DESCRIPTOR_LEN] = { 0 };
    char value[DESCRIPTOR_LEN] = { 0 };
    if (seedtool_account_xpub(mnemonic, passphrase, type, account, SEEDTOOL_XPUB, xpub, sizeof(xpub))
        == SEEDTOOL_OK) {
        (void)snprintf(body, sizeof(body), "%s([%s/%u'/0'/%u']%s/<0;1>/*)", type == SEEDTOOL_BIP84 ? "wpkh" : "tr",
            fphex, (unsigned)type, (unsigned)account, xpub);
        seedtool_zero(xpub, sizeof(xpub));
        if (seedtool_descriptor_checksum(body, value, sizeof(value)) == SEEDTOOL_OK) {
            if (page_text("Descriptor", value)) {
                show_account_key_qr("Descriptor", value);
            }
        } else {
            notice("Error", "Could not derive", NULL);
        }
    } else {
        seedtool_zero(xpub, sizeof(xpub));
        notice("Error", "Could not derive", NULL);
    }
    seedtool_zero(body, sizeof(body));
    seedtool_zero(value, sizeof(value));
}

/* An address's QR with its path and the address beside it. A taproot address
 * (62 characters) does not fit the 48-character margin, so it falls back to the
 * code alone, then to paged text. */
/* The chord on the sun cycles how bright the code's light half is drawn, for
 * phones that will not lock onto white. Up and down move between the sun and
 * the arrow. */
static void show_address_qr(const char* title, const char* address)
{
    size_t selected = SEEDTOOL_NAV_BACK;
    size_t fallback = SEEDTOOL_NAV_BACK;
    for (;;) {
        if (show_qr(seedtool_render_qr_address(title, address, selected))) {
            const seedtool_key_t key = wait_key();
            if (key == KEY_PREV || key == KEY_NEXT) {
                selected = selected == SEEDTOOL_NAV_BACK ? SEEDTOOL_NAV_SHADE : SEEDTOOL_NAV_BACK;
                continue;
            }
            if (key == KEY_REDRAW) {
                continue;
            }
            if (key == KEY_SELECT && selected == SEEDTOOL_NAV_SHADE) {
                seedtool_render_qr_cycle_shade();
                continue;
            }
            return;
        }
        if (!show_qr(seedtool_render_qr(title, address, fallback))) {
            notice("Too long for a QR", title, "Read it as text instead");
            return;
        }
        /* The fallback answers the same keys; the chord on the arrow goes on to
         * the text. */
        for (;;) {
            const seedtool_key_t key = wait_key();
            if (key == KEY_PREV || key == KEY_NEXT) {
                fallback = fallback == SEEDTOOL_NAV_BACK ? SEEDTOOL_NAV_SHADE : SEEDTOOL_NAV_BACK;
                break;
            }
            if (key == KEY_REDRAW) {
                break;
            }
            if (key == KEY_SELECT && fallback == SEEDTOOL_NAV_SHADE) {
                seedtool_render_qr_cycle_shade();
                break;
            }
            (void)page_text_impl(title, address, false, true);
            return;
        }
        continue;
    }
}

/* The Addresses screen's cache: derived once per visit rather than on every
 * return from a QR, which would repeat PBKDF2. Addresses are public, so keeping
 * them for the visit trades privacy, not the seed. */
static char addresses[ADDRESS_LIST_ROWS][SEEDTOOL_MAX_ADDRESS_LEN];
static char address_labels[ADDRESS_SHOWN_ROWS][ADDRESS_LABEL_LEN];
static const char* address_items[ADDRESS_SHOWN_ROWS + 2]; /* + Go to index + Back */

/* Fills the cache for one type, account and branch: every index is derived (Go
 * to index can reach any), the first ADDRESS_SHOWN_ROWS labelled. Returns
 * false, having told the reader, on a derivation error. */
static bool derive_addresses(const char* mnemonic, const char* passphrase, const seedtool_address_type_t type,
    const uint32_t account, const seedtool_chain_t chain, const char* const prefix)
{
    /* Cleared on the way in too, so a failed derivation leaves no earlier rows. */
    seedtool_zero(address_labels, sizeof(address_labels));
    screen_text(prefix, "Deriving addresses...", NULL, NULL);
    if (seedtool_mainnet_addresses(mnemonic, passphrase, type, account, chain, ADDRESS_LIST_ROWS, addresses)
        != SEEDTOOL_OK) {
        seedtool_zero(addresses, sizeof(addresses));
        notice("Error", "Could not derive addresses", NULL);
        return false;
    }
    for (uint32_t i = 0; i < ADDRESS_SHOWN_ROWS; ++i) {
        /* %.*s: GCC's format-truncation analysis loses the bound on
         * addresses[i]. */
        (void)snprintf(address_labels[i], ADDRESS_LABEL_LEN, "%3u  %.*s", (unsigned)i,
            (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1), addresses[i]);
        address_items[i] = address_labels[i];
    }
    address_items[ADDRESS_SHOWN_ROWS] = "Go to index";
    return true;
}

/* Browses the cache. Returns the chosen index with its address in address_out,
 * or -1 on Back or timeout. `cursor` is where the list opens and where it is
 * left; Go to index moves it only within the shown rows. */
static int browse_addresses(
    const char* const prefix, char* address_out, const size_t address_out_len, size_t* cursor)
{
    for (;;) {
        const int selected = choose_menu_at(prefix, address_items, ADDRESS_SHOWN_ROWS + 1, cursor);
        if (selected == (int)ADDRESS_SHOWN_ROWS) {
            uint32_t index = 0;
            const int result = enter_address_index(&index);
            if (result < 0) {
                return -1;
            }
            if (result == 0) {
                continue;
            }
            /* `index` addresses the static cache directly, so it is checked
             * here even though the keypad cannot type past it. */
            if (index > SEEDTOOL_MAX_ADDRESS_INDEX) {
                continue;
            }
            if (index < ADDRESS_SHOWN_ROWS) {
                *cursor = index;
            }
            /* %.*s, for the same reason as in derive_addresses. */
            (void)snprintf(address_out, address_out_len, "%.*s", (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1),
                addresses[index]);
            return (int)index;
        }
        /* choose_menu_at has already left the cursor on what was taken. */
        if (selected >= 0 && selected < (int)ADDRESS_SHOWN_ROWS) {
            (void)snprintf(address_out, address_out_len, "%.*s", (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1),
                addresses[selected]);
        }
        return selected < 0 || selected == (int)ADDRESS_SHOWN_ROWS + 1 ? -1 : selected;
    }
}

/* Every script type the viewer derives, in the order offered; everything that
 * names a type reads it here. */
static const struct {
    seedtool_address_type_t type;
    const char* name;
} ADDRESS_TYPES[] = {
    { SEEDTOOL_BIP84, "Native SegWit" },
    { SEEDTOOL_BIP86, "Taproot" },
};
#define ADDRESS_TYPE_COUNT (sizeof(ADDRESS_TYPES) / sizeof(ADDRESS_TYPES[0]))

static const char* address_type_name(const seedtool_address_type_t type)
{
    for (size_t i = 0; i < ADDRESS_TYPE_COUNT; ++i) {
        if (ADDRESS_TYPES[i].type == type) {
            return ADDRESS_TYPES[i].name;
        }
    }
    return "";
}

/* Which script type to derive, opened on the one in force. Returns false,
 * leaving `*type` alone, on Back or a timeout. */
static bool choose_address_type(seedtool_address_type_t* type)
{
    char labels[ADDRESS_TYPE_COUNT][32];
    const char* items[ADDRESS_TYPE_COUNT + 1];
    size_t current = 0;
    for (size_t i = 0; i < ADDRESS_TYPE_COUNT; ++i) {
        /* The enumerator is the BIP number itself, so the label states it
         * rather than the table repeating it as text. */
        (void)snprintf(
            labels[i], sizeof(labels[i]), "%s (BIP%u)", ADDRESS_TYPES[i].name, (unsigned)ADDRESS_TYPES[i].type);
        items[i] = labels[i];
        if (ADDRESS_TYPES[i].type == *type) {
            current = i;
        }
    }
    const int selected = choose_menu_at("Type", items, ADDRESS_TYPE_COUNT, &current);
    if (selected < 0 || (size_t)selected >= ADDRESS_TYPE_COUNT) {
        return false;
    }
    *type = ADDRESS_TYPES[selected].type;
    return true;
}

/* The watch-only account key for the type and account currently set, in the
 * format asked for. */
static void show_account_key(const char* mnemonic, const char* passphrase, const char* fphex,
    const seedtool_address_type_t type, const uint32_t account, const seedtool_key_format_t format)
{
    char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
    char origin[32];
    (void)snprintf(origin, sizeof(origin), "[%s/%u'/0'/%u']", fphex, (unsigned)type, (unsigned)account);
    if (seedtool_account_xpub(mnemonic, passphrase, type, account, format, xpub, sizeof(xpub)) == SEEDTOOL_OK) {
        if (page_text(origin, xpub)) {
            export_qr(mnemonic, passphrase, fphex, type, account, format);
        }
    } else {
        notice("Error", "Could not derive account key", NULL);
    }
    seedtool_zero(xpub, sizeof(xpub));
}

/* xpub, zpub and the descriptor: the same key three ways, behind one row since
 * each reveals every address. SLIP-132 has no taproot prefix, so zpub is BIP84
 * only. */
static void show_extended_keys(const char* mnemonic, const char* passphrase, const char* fphex,
    const seedtool_address_type_t type, const uint32_t account)
{
    size_t cursor = 0;
    for (;;) {
        const char* items[4];
        size_t count = 0;
        items[count++] = "xpub";
        if (type == SEEDTOOL_BIP84) {
            items[count++] = "zpub";
        }
        items[count++] = "Descriptor";
        const int selected = choose_menu_at("Extended public key", items, count, &cursor);
        /* The arrow returns `count`, one past the last row. */
        if (selected < 0 || (size_t)selected == count) {
            return;
        }
        if (selected == 0) {
            show_account_key(mnemonic, passphrase, fphex, type, account, SEEDTOOL_XPUB);
        } else if (type == SEEDTOOL_BIP84 && selected == 1) {
            show_account_key(mnemonic, passphrase, fphex, type, account, SEEDTOOL_ZPUB);
        } else {
            show_descriptor(mnemonic, passphrase, fphex, type, account);
        }
    }
}

/* The address list for the type and account currently set, on whichever
 * branch is asked for first. */
static void show_addresses(
    const char* mnemonic, const char* passphrase, const seedtool_address_type_t type, const uint32_t account)
{
    /* Which branch, asked before the list, as xpub/zpub is. */
    const char* const branches[] = { "Receive", "Change" };
    const int branch = choose_menu("Addresses", branches, 2);
    if (branch < 0 || branch == 2) {
        return;
    }
    const seedtool_chain_t chain = branch == 0 ? SEEDTOOL_RECEIVE : SEEDTOOL_CHANGE;
    /* The path every row shares, as the title, so a long list never loses what
     * it shows. Each address's own path is this plus its index. */
    char prefix[24];
    (void)snprintf(
        prefix, sizeof(prefix), "m/%u'/0'/%u'/%u", (unsigned)type, (unsigned)account, (unsigned)chain);
    if (!derive_addresses(mnemonic, passphrase, type, account, chain, prefix)) {
        return;
    }
    /* Back to the list after each address, with the cursor where it was; only
     * backing out of the list leaves. */
    size_t cursor = 0;
    for (;;) {
        char address[SEEDTOOL_MAX_ADDRESS_LEN] = { 0 };
        const int index = browse_addresses(prefix, address, sizeof(address), &cursor);
        if (index < 0) {
            seedtool_zero(address, sizeof(address));
            break;
        }
        /* Sized for a ten-digit index: the bound browse_addresses applies is
         * invisible to -Wformat-truncation. */
        char path[sizeof(prefix) + 12];
        (void)snprintf(path, sizeof(path), "%s/%u", prefix, (unsigned)index);
        /* One screen: the code, its path and the address. */
        show_address_qr(path, address);
        seedtool_zero(address, sizeof(address));
    }
    seedtool_zero(addresses, sizeof(addresses));
    seedtool_zero(address_labels, sizeof(address_labels));
    /* A path, not a secret; wiped anyway, like every `prefix` in this file. */
    seedtool_zero(prefix, sizeof(prefix));
}

/* One word per screen as a Stackbit 1248 plate, which restores through "Enter
 * word numbers". */
static void show_stackbit(const char* mnemonic)
{
    uint16_t numbers[24];
    size_t count = 0;
    if (seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &count) != SEEDTOOL_OK) {
        notice("Error", "Could not compute", "word numbers");
        return;
    }
    const char* const layouts[] = { "Simple grid", "Physical layout" };
    const int layout = choose_menu("Stackbit 1248", layouts, 2);
    if (layout < 0 || layout == 2) {
        seedtool_zero(numbers, sizeof(numbers));
        return;
    }
    /* The paged screens' ring with words for pages: 0 is the arrow, 1..count
     * the words. There is nothing to accept, so no confirm. */
    size_t position = 1;
    for (;;) {
        const size_t selected = page_shown(position, count);
        char footer[16];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(selected + 1), (unsigned)count);
        const char* const word = seedtool_word(numbers[selected] - 1);
        const seedtool_nav_t nav = page_nav(position, count, NULL, false, SEEDTOOL_CONFIRM_TICK);
        if (layout == 0) {
            seedtool_render_stackbit_screen(&nav, "Stackbit 1248", numbers[selected], word, footer);
        } else {
            seedtool_render_stackbit_physical_screen(&nav, "Stackbit 1248", numbers[selected], word, footer);
        }
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            /* On the arrow the chord leaves; on a word it reads on, the same
             * answer page_text gives the chord on a page. */
            if (!position) {
                seedtool_zero(numbers, sizeof(numbers));
                return;
            }
            position = page_step(position, count, true, false);
            break;
        case KEY_NEXT:
            position = page_step(position, count, true, false);
            break;
        case KEY_PREV:
            position = page_step(position, count, false, false);
            break;
        case KEY_REDRAW:
            break;
        default:
            seedtool_zero(numbers, sizeof(numbers));
            return;
        }
    }
}

/* Every word numbered as a backup numbers them, four to a page (12 and 24 both
 * divide by four), so a word is found by its page. `show_words` picks "position
 * word" or "position number". */
/* Returns true on the confirm or paging past the end, false on backing out;
 * `confirmable` decides whether there is a confirm. */
static bool show_numbered_list_impl(const char* mnemonic, const bool show_words, const bool confirmable)
{
    uint16_t numbers[24];
    size_t count = 0;
    if (seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &count) != SEEDTOOL_OK) {
        notice("Error", "Could not compute", "word numbers");
        return false;
    }
    const size_t pages = (count + 3) / 4;
    /* Opens on the first page. `cursor`, since `position` is the word's place. */
    size_t cursor = 1;
    bool advanced;
    /* Outside the loop so the one exit can wipe them. */
    char lines[4][24] = { { 0 } };
    for (;;) {
        memset(lines, 0, sizeof(lines));
        const size_t page = page_shown(cursor, pages);
        const size_t first = page * 4;
        for (size_t i = 0; i < 4 && first + i < count; ++i) {
            const size_t position = first + i + 1;
            const uint16_t number = numbers[first + i];
            /* Zero-padded, not space-padded: this font's digits share one width
             * and its space does not, so the words line up. */
            if (show_words) {
                (void)snprintf(lines[i], sizeof(lines[i]), "%02u. %s", (unsigned)position, seedtool_word(number - 1));
            } else {
                (void)snprintf(lines[i], sizeof(lines[i]), "%02u. %04u", (unsigned)position, (unsigned)number);
            }
        }
        /* Sized for ten-digit counts: -Wformat-truncation cannot see the real
         * bound, and it is an error in the firmware build. */
        char footer[24];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
        const seedtool_nav_t nav = page_nav(cursor, pages, footer, confirmable, SEEDTOOL_CONFIRM_TICK);
        const char* const rows[] = { lines[0], lines[1], lines[2], lines[3] };
        seedtool_render_nav_rows(&nav, show_words ? "BIP39 words" : "BIP39 word numbers", rows, 4);
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            if (!cursor) {
                advanced = false;
                goto done;
            }
            if (cursor > pages) {
                advanced = true;
                goto done;
            }
            cursor = page_step(cursor, pages, true, confirmable);
            break;
        case KEY_NEXT:
            cursor = page_step(cursor, pages, true, confirmable);
            break;
        case KEY_PREV:
            cursor = page_step(cursor, pages, false, confirmable);
            break;
        case KEY_REDRAW:
            break;
        default:
            advanced = false;
            goto done;
        }
    }
done:
    seedtool_zero(numbers, sizeof(numbers));
    seedtool_zero(lines, sizeof(lines));
    return advanced;
}

/* The list as a step in a sequence: the caller goes on to the next screen only
 * if this one was seen through, so it ends on a tick. */
static bool show_numbered_list(const char* mnemonic, const bool show_words)
{
    return show_numbered_list_impl(mnemonic, show_words, true);
}

/* The list as something to look at from the backup menu, which does nothing
 * with the answer either way. Down the pages and out by the arrow. */
static void read_numbered_list(const char* mnemonic, const bool show_words)
{
    (void)show_numbered_list_impl(mnemonic, show_words, false);
}

/* A Compact SeedQR is the whole seed, so the warning is starker than the
 * account key's. tools/origo_verify.py inspect prints the same payload for an
 * independent check. */
/* Views: 0 the full code, 1 the region map, 2..regions+1 Krux-style zoomed
 * tiles. One past the last is the sun, the shade control, which keeps the last
 * view on screen. */
static void export_seed_qr(const char* mnemonic)
{
    if (!nav_acknowledge(
            "Compact SeedQR", "Encodes your ENTIRE seed", "A photo = total loss of funds", "Show QR", true)) {
        return;
    }
    uint8_t entropy[SEEDTOOL_HASH_LEN] = { 0 };
    size_t len = 0;
    if (seedtool_mnemonic_entropy(mnemonic, entropy, sizeof(entropy), &len) == SEEDTOOL_OK) {
        const size_t regions = seedtool_render_qr_bytes_regions(len);
        const size_t views = regions + 2;
        const size_t steps = views + 1;
        size_t cursor = 0;
        size_t view = 0;
        for (;;) {
            const size_t chrome = cursor < views ? SEEDTOOL_NAV_BACK : SEEDTOOL_NAV_SHADE;
            const bool ok = show_qr(view == 0 ? seedtool_render_qr_bytes("Compact SeedQR", entropy, len, chrome)
                    : view == 1 ? seedtool_render_qr_bytes_map("Compact SeedQR", entropy, len, chrome)
                                : seedtool_render_qr_bytes_region("Compact SeedQR", entropy, len, view - 2, chrome));
            if (!ok) {
                notice("Too long for a QR", "Compact SeedQR", "Read it as text instead");
                break;
            }
            const seedtool_key_t key = wait_key();
            if (key == KEY_SELECT && cursor == views) {
                seedtool_render_qr_cycle_shade();
                continue;
            }
            switch (key) {
            case KEY_PREV:
                cursor = (cursor + steps - 1) % steps;
                break;
            case KEY_NEXT:
                cursor = (cursor + 1) % steps;
                break;
            case KEY_REDRAW:
                break;
            default:
                goto done;
            }
            /* The sun is the one stop that shows no view of its own, so it
             * leaves the last one standing rather than blanking the screen. */
            if (cursor < views) {
                view = cursor;
            }
        }
    } else {
        notice("Error", "Could not derive entropy", NULL);
    }
done:
    seedtool_zero(entropy, sizeof(entropy));
}

static void show_backup_menu(const char* mnemonic)
{
    size_t cursor = 0;
    for (;;) {
        const char* items[] = { "Words", "Numbers", "Stackbit 1248", "Compact SeedQR" };
        const int selected = choose_menu_at("Backup", items, 4, &cursor);
        if (selected < 0 || selected == 4) {
            return;
        }
        if (selected == 0) {
            read_numbered_list(mnemonic, true);
        } else if (selected == 1) {
            read_numbered_list(mnemonic, false);
        } else if (selected == 2) {
            show_stackbit(mnemonic);
        } else {
            export_seed_qr(mnemonic);
        }
    }
}

/* Derivation: the passphrase, the script type and the account, the inputs to
 * everything the wallet derives. `fp`/`fphex` come in by pointer because the
 * passphrase changes the fingerprint titling the wallet. */
static void show_derivation_menu(const char* mnemonic, uint32_t* account, seedtool_address_type_t* type,
    char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1], uint8_t fp[4], char fphex[9])
{
    size_t cursor = 0;
    for (;;) {
        char account_item[16];
        (void)snprintf(account_item, sizeof(account_item), "Account: %u", (unsigned)*account);
        char type_item[32];
        (void)snprintf(type_item, sizeof(type_item), "Type: %s", address_type_name(*type));
        /* Says whether one is set, never anything about what it is. */
        const char* const passphrase_item = passphrase[0] ? "Passphrase: session only" : "Passphrase: none";
        /* Widest consequence first: the passphrase changes the seed, the type
         * the path, the account a branch of it. */
        const char* items[] = { passphrase_item, type_item, account_item };
        const int selected = choose_menu_at("Derivation", items, 3, &cursor);
        if (selected < 0 || selected == 3) {
            return;
        }
        if (selected == 0) {
            if (edit_session_passphrase(passphrase)) {
                if (seedtool_master_fingerprint(mnemonic, passphrase, fp) != SEEDTOOL_OK) {
                    notice("Error", "Derivation failed", NULL);
                } else {
                    hexstr(fp, 4, fphex);
                }
            }
        } else if (selected == 1) {
            (void)choose_address_type(type);
        } else {
            uint32_t chosen = *account;
            if (enter_account(&chosen) == 1) {
                *account = chosen;
            }
        }
    }
}

static void show_wallet_data(const char* mnemonic)
{
    uint8_t fp[4] = { 0 };
    char fphex[9] = { 0 };
    char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1] = { 0 };

    /* No passphrase to start. It is set from Derivation like the account and
     * type, and the fingerprint in the title shows which seed is in force. */
    if (seedtool_master_fingerprint(mnemonic, passphrase, fp) != SEEDTOOL_OK) {
        notice("Error", "Derivation failed", NULL);
        goto done;
    }
    hexstr(fp, sizeof(fp), fphex);

    /* The path, live for the whole session: set in Derivation and used by every
     * derived screen. */
    uint32_t account = 0;
    seedtool_address_type_t type = SEEDTOOL_BIP84;
    /* The fingerprint titles the menu, so a reader who knows their wallet's
     * sees at a glance which one is being derived. */
    char wallet_title[sizeof("Wallet @") + 8];
    _Static_assert(sizeof(wallet_title) >= sizeof("Wallet @") + 8,
        "the wallet title must hold its label and all eight fingerprint hex digits");
    (void)snprintf(wallet_title, sizeof(wallet_title), "Wallet @%s", fphex);
    size_t cursor = 0;
    for (;;) {
        const char* menu[] = { "Backup", "Extended public key", "Derivation", "Addresses", "Erase and restart" };
        /* No arrow: the only exit erases the session, so it is a row. */
        const int selected
            = choose_nav(wallet_title, menu, sizeof(menu) / sizeof(menu[0]), NULL, false, false, &cursor);
        /* The only way out of a wallet session is a restart: C startup zeroes
         * .bss, and scrub_free_heap zeroes the old stacks, which a reset
         * alone leaves in DRAM. Display settings go back to their defaults. */
        if (selected < 0 || selected == 4) {
            seedtool_zero(fp, sizeof(fp));
            seedtool_zero(fphex, sizeof(fphex));
            seedtool_zero(passphrase, sizeof(passphrase));
            seedtool_platform_restart();
        }
        switch (selected) {
        case 0:
            show_backup_menu(mnemonic);
            break;
        case 1:
            show_extended_keys(mnemonic, passphrase, fphex, type, account);
            break;
        case 2:
            show_derivation_menu(mnemonic, &account, &type, passphrase, fp, fphex);
            /* The passphrase may have changed under it, and with it the
             * fingerprint this title states. */
            (void)snprintf(wallet_title, sizeof(wallet_title), "Wallet @%s", fphex);
            break;
        default:
            show_addresses(mnemonic, passphrase, type, account);
            break;
        }
    }
done:
    seedtool_zero(fp, sizeof(fp));
    seedtool_zero(fphex, sizeof(fphex));
    seedtool_zero(passphrase, sizeof(passphrase));
}

/* A backup is only as good as its transcription. Quizzes the middle word of
 * every three, as Jade does (process/mnemonic.c): 4 checks for 12 words, 8 for
 * 24. The middle rather than a random word, since no screen here uses the RNG.
 * A wrong word restarts the quiz; backing out of a word steps to the previous
 * one, and out of the first leaves. Returns 1 once a pass matches, 0 on giving
 * up, -1 on timeout. */
static int confirm_backup(const char* mnemonic, const size_t count)
{
    /* The canonical parser, which validates every word. */
    uint16_t numbers[24];
    size_t total = 0;
    if (seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &total) != SEEDTOOL_OK) {
        return -1;
    }
    const size_t checks = count / 3;
    for (;;) {
        bool wrong = false;
        size_t n = 0;
        while (n < checks) {
            const size_t index = n * 3 + 1;
            const char* const expected = seedtool_word(numbers[index] - 1);
            char typed[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
            /* Unnarrowed on purpose: the quiz asks what the reader wrote down,
             * and the words it asks for are never the last one anyway. */
            const int result = enter_word(index + 1, count, NULL, typed, sizeof(typed));
            if (result < 0) {
                seedtool_zero(typed, sizeof(typed));
                seedtool_zero(numbers, sizeof(numbers));
                return -1;
            }
            if (result == 0) {
                seedtool_zero(typed, sizeof(typed));
                if (!n) {
                    seedtool_zero(numbers, sizeof(numbers));
                    return 0;
                }
                --n;
                continue;
            }
            const bool matched = strcmp(typed, expected) == 0;
            seedtool_zero(typed, sizeof(typed));
            if (!matched) {
                wrong = true;
                break;
            }
            ++n;
        }
        if (!wrong) {
            seedtool_zero(numbers, sizeof(numbers));
            notice("Backup confirmed", "Words matched", NULL);
            return 1;
        }
        if (!nav_acknowledge("Word doesn't match", "Check your backup", NULL, "Try again", false)) {
            seedtool_zero(numbers, sizeof(numbers));
            return 0;
        }
    }
}

/* The words, the reminder, the quiz and the wallet it unlocks, each back
 * stepping one stage. Returns false only when the reader steps back off the
 * first. */
static bool review_backup_and_show_wallet(const char* mnemonic, const size_t words)
{
    enum { STAGE_WORDS, STAGE_INTRO, STAGE_QUIZ, STAGE_DONE };
    int stage = STAGE_WORDS;
    bool stepped_back = false;
    while (stage != STAGE_DONE) {
        switch (stage) {
        case STAGE_WORDS:
            if (!show_numbered_list(mnemonic, true)) {
                stepped_back = true;
                stage = STAGE_DONE;
                break;
            }
            stage = STAGE_INTRO;
            break;
        case STAGE_INTRO: {
            /* Says why the words are about to leave. Sized for ten-digit
             * counts, since -Wformat-truncation is an error in the firmware
             * build. */
            char intro[48];
            (void)snprintf(
                intro, sizeof(intro), "Retype %u of the %u words", (unsigned)(words / 3), (unsigned)words);
            const int taken
                = nav_screen("Confirm backup", intro, "Have your backup ready", "Start quiz", true, false, NULL);
            /* A timeout ends it rather than repainting the mnemonic on the way
             * out. */
            stage = taken == NAV_TIMEOUT ? STAGE_DONE : taken == NAV_CONFIRM ? STAGE_QUIZ : STAGE_WORDS;
            break;
        }
        case STAGE_QUIZ: {
            const int outcome = confirm_backup(mnemonic, words);
            if (outcome == 1) {
                show_wallet_data(mnemonic);
                stage = STAGE_DONE;
            } else {
                /* Backing out of the quiz's first word lands on the intro,
                 * one stage, as everywhere else. */
                stage = outcome < 0 ? STAGE_DONE : STAGE_INTRO;
            }
            break;
        }
        default:
            stage = STAGE_DONE;
            break;
        }
    }
    return !stepped_back;
}

/* Returns false only when the reader steps back off the very first screen -
 * the caller still holds the entropy that produced this, and throwing it away
 * to start the run over is not what one press should mean. */
static bool show_generated(seedtool_generated_t* generated)
{
    char hash[65];
    hexstr(generated->hash, sizeof(generated->hash), hash);
    enum { STAGE_TRANSCRIPT, STAGE_HASH, STAGE_REVIEW, STAGE_DONE };
    int stage = STAGE_TRANSCRIPT;
    bool stepped_back = false;
    while (stage != STAGE_DONE) {
        switch (stage) {
        case STAGE_TRANSCRIPT:
            if (!page_text("Canonical transcript", generated->transcript)) {
                stepped_back = true;
                stage = STAGE_DONE;
                break;
            }
            stage = STAGE_HASH;
            break;
        case STAGE_HASH:
            stage = page_text("SHA256", hash) ? STAGE_REVIEW : STAGE_TRANSCRIPT;
            break;
        case STAGE_REVIEW:
            stage = review_backup_and_show_wallet(generated->mnemonic, generated->words) ? STAGE_DONE : STAGE_HASH;
            break;
        default:
            stage = STAGE_DONE;
            break;
        }
    }
    seedtool_zero(hash, sizeof(hash));
    return !stepped_back;
}

/* Bits so far and whether the run looks patterned, for every source, so the
 * live bar and the end-of-run gate agree. Cards without replacement are exact;
 * the rest go through the Shannon estimator on a 1-indexed copy of the values. */
static void entropy_quality(
    const seedtool_source_t source, const uint8_t* values, const size_t count, int* bits_out, bool* pattern_out)
{
    if (source == SEEDTOOL_CARDS) {
        (void)seedtool_card_entropy_bits(count, bits_out);
        (void)seedtool_card_pattern_detected(values, count, pattern_out);
        return;
    }
    /* Zeroed: only faces[0..count) is filled. */
    uint8_t faces[SEEDTOOL_MAX_EVENTS] = { 0 };
    const uint8_t* faces_ptr = values;
    if (source == SEEDTOOL_COIN || source == SEEDTOOL_CARDS_REPLACE) {
        for (size_t i = 0; i < count; ++i) {
            faces[i] = (uint8_t)(values[i] + 1);
        }
        faces_ptr = faces;
    }
    (void)seedtool_dice_entropy_bits(source, faces_ptr, count, bits_out);
    (void)seedtool_dice_pattern_detected(source, faces_ptr, count, pattern_out);
    /* Skipped at zero draws: there is no distribution yet to be biased. */
    if (count) {
        *bits_out += (int)lround(seedtool_dice_entropy_bias_bits(source));
    }
    /* A copy of the entropy, made on every keystroke. */
    seedtool_zero(faces, sizeof(faces));
}

/* Live quality readout: draws and bits so far against what the seed needs,
 * adapted from Krux (src/krux/pages/new_mnemonic/dice_rolls.py). Display only;
 * nothing here is hashed. */
static seedtool_progress_t entropy_progress(const seedtool_source_t source, const uint8_t* values, const size_t count,
    const size_t required, const size_t min_bits)
{
    int bits = 0;
    bool pattern = false;
    entropy_quality(source, values, count, &bits, &pattern);
    const int capped_bits = (size_t)bits < min_bits ? bits : (int)min_bits;
    const seedtool_progress_t progress = {
        .rolls_pct = (int)(count * 100 / required),
        .entropy_pct = (int)((size_t)capped_bits * 100 / min_bits),
        .warn = pattern,
        .complete = false, /* only called for count < required, so never yet */
        .graded = true,
        .bits = bits,
    };
    return progress;
}

/* Collects the whole transcript and generates from it. Returns 1 when a seed was
 * produced, 0 when the user backed out of the first entry, -1 on timeout. */
static int collect_entropy(const int source, const size_t words)
{
    /* "Coins", not "Coin flips": the longest name with the longest counter did
     * not fit the title. */
    static const char* const names[] = { "D6 dice", "D20 dice", "Coins", "Cards", "Cards" };
    static const char* const nouns[] = { "rolls", "rolls", "flips", "cards", "cards" };
    const size_t required = seedtool_required_events((seedtool_source_t)source, words);
    const unsigned max = source == SEEDTOOL_D6 ? 6 : 20;
    const size_t min_bits = seedtool_min_entropy_bits(words);

    uint8_t values[SEEDTOOL_MAX_EVENTS] = { 0 };
    bool available[52];
    char history[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1] = { 0 };
    seedtool_generated_t generated;
    int outcome = 1;
    size_t i = 0;
    memset(available, 1, sizeof(available));
    memset(&generated, 0, sizeof(generated));

    {
        /* Shown once, with the bar already in place but empty, so its shape and
         * position are seen before they start moving. */
        char needed[24];
        (void)snprintf(needed, sizeof(needed), "%u %s needed", (unsigned)required, nouns[source]);
        /* One deck without replacement cannot reach 256 bits, so 24-word cards
         * say how they are drawn. One line, since two wrapped into the bar. */
        const char* const hint
            = source == SEEDTOOL_CARDS_REPLACE ? "Return & reshuffle each card" : "Red bar = non-random";
        const seedtool_progress_t empty = { 0 };
        if (!nav_dice_confirm(names[source], needed, hint, "Start", false, &empty)) {
            outcome = 0;
        }
    }

    for (; outcome == 1;) {
        while (i < required) {
            unsigned value = 0;
            /* Rebuilt from the values each time, so a step back unwrites the
             * last entry too. */
            if (seedtool_transcript((seedtool_source_t)source, values, i, history, sizeof(history)) != SEEDTOOL_OK) {
                history[0] = '\0';
            }
            const size_t shown = strlen(history) - seedtool_render_fit_tail(history);
            const seedtool_progress_t progress
                = entropy_progress((seedtool_source_t)source, values, i, required, min_bits);
            const char* const label = names[source];
            int result;
            if (source == SEEDTOOL_COIN) {
                result = enter_coin_flip(label, (unsigned)(i + 1), (unsigned)required, &value, history + shown,
                    &progress);
            } else if (source == SEEDTOOL_CARDS || source == SEEDTOOL_CARDS_REPLACE) {
                /* With replacement nothing is excluded. */
                result = enter_card((unsigned)(i + 1), (unsigned)required, &value,
                    source == SEEDTOOL_CARDS ? available : NULL, history + shown, &progress);
            } else {
                result = enter_value(label, (unsigned)(i + 1), (unsigned)required, 1, max, &value, NULL, NULL,
                    history + shown, &progress);
            }
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
        if (outcome != 1) {
            break;
        }
        /* The run is fixed-length: declining steps back into the entry at the
         * last value. */
        int bits = 0;
        bool pattern = false;
        entropy_quality((seedtool_source_t)source, values, required, &bits, &pattern);
        const bool poor = (size_t)bits + DICE_ENTROPY_TOLERANCE < min_bits;
        bool proceed = true;
        /* Both verdicts show the number the decision was made on. */
        char bits_line[24];
        (void)snprintf(bits_line, sizeof(bits_line), "%d of %u bits", bits, (unsigned)min_bits);
        /* The verdict goes in the body, not the title: it is a finding about
         * the run, and belongs where the eye already is. */
        if (poor) {
            proceed = nav_acknowledge(NULL, "Poor entropy!", bits_line, "Proceed anyway", true);
        }
        if (proceed && pattern) {
            proceed = nav_acknowledge(NULL, "Pattern detected!", NULL, "Proceed anyway", true);
        }
        if (proceed && !poor && !pattern) {
            /* The one place `complete` is true (a green outline); the verdict
             * is also in words, for readers who cannot tell the colours apart. */
            const seedtool_progress_t complete = { .rolls_pct = 100, .entropy_pct = 100, .warn = false, .complete = true };
            proceed = nav_dice_confirm(names[source], bits_line, "Looks good", "Generate seed", false, &complete);
        }
        if (proceed) {
            if (seedtool_generate((seedtool_source_t)source, words, values, required, &generated) != SEEDTOOL_OK) {
                notice("Error", "Could not generate seed", NULL);
                break;
            }
            if (show_generated(&generated)) {
                break;
            }
            /* Back off the first screen after generating: the verdict again,
             * with every roll still here. */
            continue;
        }
        --i;
        /* Put the last card back, as the in-run back-step does, or it stays
         * struck off the deck. */
        if (source == SEEDTOOL_CARDS) {
            available[values[i]] = true;
        }
    }
    seedtool_zero(values, sizeof(values));
    seedtool_zero(available, sizeof(available));
    seedtool_zero(history, sizeof(history));
    seedtool_zero(&generated, sizeof(generated));
    return outcome;
}

/* 2048 words: eleven flips name one, every word equally likely. Most
 * significant first, the order seedtool_complete_checksum packs them in, so the
 * screen shows the arithmetic the seed is built from. */
#define COIN_WORD_BITS 11

/* Eleven flipped words are 121 bits and twenty-three are 253; the entropy needs
 * 128 and 256. The remainder is flipped loose at the end, and BIP39's checksum
 * supplies the last 4 or 8 bits of the final word. */
static size_t coin_flipped_words(const size_t words) { return words == 12 ? 11 : 23; }
static size_t coin_tail_bits(const size_t words) { return words == 12 ? 7 : 3; }

/* One word's worth of flips. Returns 1 with `*index_out` set, 0 when the reader
 * backed out of its first flip, -1 on timeout. Backing out of a later flip
 * undoes that flip only. */
static int enter_flipped_word(const unsigned position, const unsigned total, uint16_t* index_out)
{
    uint8_t bits[COIN_WORD_BITS] = { 0 };
    /* Sized for ten-digit counts, for -Wformat-truncation. */
    char title[48];
    (void)snprintf(title, sizeof(title), "Word %u of %u", position, total);
    int outcome = 1;
    size_t i = 0;
    while (i < COIN_WORD_BITS) {
        /* Rebuilt per flip, so only what is on screen is held. */
        char history[COIN_WORD_BITS + 1] = { 0 };
        for (size_t f = 0; f < i; ++f) {
            history[f] = (char)('0' + bits[f]);
        }
        unsigned bit = 0;
        const int result = enter_coin_flip(title, (unsigned)(i + 1), COIN_WORD_BITS, &bit, history, NULL);
        seedtool_zero(history, sizeof(history));
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
            continue;
        }
        bits[i++] = (uint8_t)bit;
    }
    if (outcome == 1) {
        uint16_t index = 0;
        for (size_t b = 0; b < COIN_WORD_BITS; ++b) {
            index = (uint16_t)((index << 1) | bits[b]);
        }
        *index_out = index;
    }
    seedtool_zero(bits, sizeof(bits));
    return outcome;
}

/* The flips, the one-based number they spell and the word, checkable against a
 * printed wordlist. Going back discards eleven flips, so it is asked rather
 * than done. Returns 1 to accept, 0 to flip the word again, -1 on timeout. */
static int confirm_flipped_word(const unsigned position, const unsigned total, const uint16_t index)
{
    char title[48], bitline[COIN_WORD_BITS + 1] = { 0 }, number[16];
    (void)snprintf(title, sizeof(title), "Word %u of %u", position, total);
    for (size_t b = 0; b < COIN_WORD_BITS; ++b) {
        bitline[b] = (char)('0' + ((index >> (COIN_WORD_BITS - 1 - b)) & 1u));
    }
    (void)snprintf(number, sizeof(number), "%04u", (unsigned)index + 1u);
    char named[48];
    (void)snprintf(named, sizeof(named), "%s  %s", number, seedtool_word(index));
    int outcome;
    for (;;) {
        /* Number and word on one line, so both body lines carry what is
         * checked. */
        const int key = nav_screen(title, bitline, named, "Use this word", true, false, NULL);
        if (key == NAV_TIMEOUT) {
            outcome = -1;
            break;
        }
        if (key == NAV_CONFIRM) {
            outcome = 1;
            break;
        }
        /* The question names the word it would discard. */
        char question[48], subject[32];
        (void)snprintf(question, sizeof(question), "Flip word %u again?", position);
        (void)snprintf(subject, sizeof(subject), "%s  %s", number, seedtool_word(index));
        /* Opens on the arrow: the bar throws eleven flips away. */
        const int answer = nav_screen(question, subject, "These flips are lost", "Flip again", true, true, NULL);
        seedtool_zero(subject, sizeof(subject));
        if (answer == NAV_TIMEOUT) {
            outcome = -1;
            break;
        }
        if (answer == NAV_CONFIRM) {
            outcome = 0;
            break;
        }
        /* Declined: back to the word, unchanged. */
    }
    seedtool_zero(bitline, sizeof(bitline));
    seedtool_zero(number, sizeof(number));
    return outcome;
}

/* Coins packed straight into the entropy rather than hashed: eleven flips per
 * word, then 7 or 3 loose bits, and BIP39's checksum finishes the last word,
 * through the same seedtool_complete_checksum complete_checksum uses. No
 * quality gate: eleven flips are eleven bits. Returns true once a mnemonic
 * reached the wallet or the session timed out, false for backing out before any
 * flip. */
static bool flip_words(const size_t words)
{
    const size_t word_count = coin_flipped_words(words);
    const size_t tail_count = coin_tail_bits(words);
    uint16_t indices[23] = { 0 };
    uint8_t tail[7] = { 0 };
    char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
    bool reached_wallet = false;
    int outcome = 1;
    size_t w = 0;

    /* Set when the run steps back onto a flipped word, which then reopens at
     * its confirmation rather than losing its flips. */
    bool reopen = false;

    for (;;) {
        while (w < word_count) {
            uint16_t index = indices[w];
            if (reopen) {
                reopen = false;
            } else {
                outcome = enter_flipped_word((unsigned)(w + 1), (unsigned)word_count, &index);
                if (outcome < 0) {
                    break;
                }
                if (outcome == 0) {
                    /* Back from a word's first flip reopens the word before it;
                     * only the very first flip leaves the run. */
                    if (!w) {
                        break;
                    }
                    --w;
                    reopen = true;
                    outcome = 1;
                    continue;
                }
            }
            outcome = confirm_flipped_word((unsigned)(w + 1), (unsigned)word_count, index);
            if (outcome < 0) {
                break;
            }
            if (outcome == 0) {
                /* Declined at the confirmation: flip this same word again. */
                outcome = 1;
                continue;
            }
            indices[w++] = index;
        }
        if (outcome != 1) {
            break;
        }

        /* The loose bits; backing out of the first returns to the last word. */
        bool back_to_words = false;
        size_t t = 0;
        while (t < tail_count) {
            char history[8] = { 0 };
            for (size_t f = 0; f < t; ++f) {
                history[f] = (char)('0' + tail[f]);
            }
            unsigned bit = 0;
            const int result
                = enter_coin_flip("Final bits", (unsigned)(t + 1), (unsigned)tail_count, &bit, history, NULL);
            seedtool_zero(history, sizeof(history));
            if (result < 0) {
                outcome = -1;
                break;
            }
            if (result == 0) {
                if (!t) {
                    back_to_words = true;
                    break;
                }
                --t;
                continue;
            }
            tail[t++] = (uint8_t)bit;
        }
        if (outcome != 1) {
            break;
        }
        if (back_to_words) {
            /* Reopened at its confirmation, like every step back. */
            w = word_count - 1;
            reopen = true;
            continue;
        }

        prefix[0] = '\0';
        size_t used = 0;
        for (size_t i = 0; i < word_count; ++i) {
            const char* const word = seedtool_word(indices[i]);
            const size_t len = strlen(word);
            /* Checked, not assumed: a truncated prefix would be a wrong seed. */
            if (used + len + (i ? 1 : 0) >= sizeof(prefix)) {
                outcome = -1;
                break;
            }
            if (i) {
                prefix[used++] = ' ';
            }
            memcpy(prefix + used, word, len);
            used += len;
            prefix[used] = '\0';
        }
        if (outcome != 1) {
            break;
        }

        if (seedtool_complete_checksum(prefix, tail, tail_count, completed, sizeof(completed)) != SEEDTOOL_OK) {
            notice("Error", "Could not complete", "the mnemonic");
            break;
        }
        review_backup_and_show_wallet(completed, words);
        reached_wallet = true;
        break;
    }

    seedtool_zero(indices, sizeof(indices));
    seedtool_zero(tail, sizeof(tail));
    seedtool_zero(prefix, sizeof(prefix));
    seedtool_zero(completed, sizeof(completed));
    /* Backing out of the very first flip is the one exit that has produced
     * nothing, and the only one that should leave the caller's menu open. */
    return reached_wallet || outcome != 0;
}

/* Flipped words or hashed flips. Only coins get this choice: theirs is the one
 * transcript a reader can turn into words by hand. Returns 0 for flipped words,
 * 1 for hashed, -1 for back. */
static int choose_coin_method(void)
{
    const char* methods[] = { "Flip each word", "Flip and hash" };
    const int selected = choose_menu("Coin method", methods, 2);
    return selected < 0 || selected == 2 ? -1 : selected;
}

/* Returns whether a seed was generated, rather than the reader backing out of a
 * picker. Leaving the wallet reboots, so true only comes back when the wallet
 * failed to open. */
static bool create_seed(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* sources[] = { "D6 dice", "D20 dice", "Coin flips", "Cards" };
        const int source = choose_menu_at("Entropy source", sources, sizeof(sources) / sizeof(sources[0]), &cursor);
        if (source < 0 || source == 4) {
            return false;
        }
        for (;;) {
            const char* lengths[] = { "12 words", "24 words" };
            const int length = choose_menu("Seed length", lengths, sizeof(lengths) / sizeof(lengths[0]));
            if (length < 0) {
                return false;
            }
            if (length == 2) {
                break;
            }
            const size_t words = length ? 24 : 12;
            if (source == SEEDTOOL_COIN) {
                bool run_hashed = false;
                for (;;) {
                    const int method = choose_coin_method();
                    if (method < 0) {
                        break; /* Back: the length picker, one stage. */
                    }
                    if (method == 1) {
                        run_hashed = true;
                        break;
                    }
                    if (flip_words(words)) {
                        return true;
                    }
                    /* Backed out before any entry: this menu again. */
                }
                if (!run_hashed) {
                    continue;
                }
            }
            /* 24-word cards draw with replacement (see SEEDTOOL_CARDS_REPLACE). */
            const int collect_source = length && source == SEEDTOOL_CARDS ? SEEDTOOL_CARDS_REPLACE : source;
            if (collect_entropy(collect_source, words) != 0) {
                return true;
            }
        }
    }
}

/* Same contract as create_seed(). */
static bool complete_checksum(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* lengths[] = { "11 words + 7 coins", "23 words + 3 coins" };
        const int selected = choose_menu_at("Complete checksum", lengths, 2, &cursor);
        if (selected < 0 || selected == 2) {
            return false;
        }
        const size_t count = selected ? 23 : 11;
        const size_t bits_count = selected ? 3 : 7;
        char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        char completed[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        uint8_t bits[7] = { 0 };
        /* Held here so stepping back from the flips hands the words back, with
         * the method they were typed by. */
        char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
        bool by_number = false;
        int outcome = enter_mnemonic_words(count, words, &by_number);
        if (outcome == 1 && !join_words(words, count, prefix, sizeof(prefix))) {
            outcome = -1;
        }
        /* Flips and result in one loop, so backing off the result is the last
         * flip again. */
        size_t i = 0;
        while (outcome == 1) {
            while (outcome == 1 && i < bits_count) {
                unsigned bit = 0;
                char flips[8] = { 0 };
                for (size_t f = 0; f < i; ++f) {
                    flips[f] = (char)('0' + bits[f]);
                }
                const int result
                    = enter_coin_flip("Coin flip", (unsigned)(i + 1), (unsigned)bits_count, &bit, flips, NULL);
                seedtool_zero(flips, sizeof(flips));
                if (result < 0) {
                    outcome = -1;
                } else if (result == 0) {
                    /* Back from the first flip returns to the list of words. */
                    if (!i) {
                        outcome = revisit_prefix(words, count, &by_number);
                        if (outcome == 1 && !join_words(words, count, prefix, sizeof(prefix))) {
                            outcome = -1;
                        }
                    } else {
                        --i;
                    }
                } else {
                    bits[i] = (uint8_t)bit;
                    ++i;
                }
            }
            if (outcome != 1) {
                break;
            }
            if (seedtool_complete_checksum(prefix, bits, bits_count, completed, sizeof(completed)) != SEEDTOOL_OK) {
                /* Said, not fallen through silently. */
                notice("Error", "Could not complete", "the checksum");
                break;
            }
            if (page_text("Completed mnemonic", completed)) {
                show_wallet_data(completed);
                break;
            }
            /* Back off the completed mnemonic: the last flip again, with the
             * words and every flip before it still in hand. */
            --i;
        }
        seedtool_zero(words, sizeof(words));
        seedtool_zero(prefix, sizeof(prefix));
        seedtool_zero(completed, sizeof(completed));
        seedtool_zero(bits, sizeof(bits));
        if (outcome != 0) {
            return true;
        }
    }
}

/* A new seed from entropy, or finishing one whose last word is missing (from a
 * Stackbit backup, say). Both end in the wallet. */
static void show_new_seed_menu(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* items[] = { "From entropy", "Complete checksum" };
        const int selected = choose_menu_at("New Seed", items, 2, &cursor);
        if (selected < 0 || selected == 2) {
            return;
        }
        /* A generated seed returns home. Leaving the wallet reboots, so this
         * only runs when the wallet failed to open. */
        if (selected == 0 ? create_seed() : complete_checksum()) {
            return;
        }
    }
}

static void restore_seed(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* lengths[] = { "12 words", "24 words" };
        const int selected = choose_menu_at("Restore mnemonic", lengths, 2, &cursor);
        if (selected < 0 || selected == 2) {
            return;
        }
        const size_t count = selected ? 24 : 12;
        char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        /* Held here, like complete_checksum's, so that every step back from
         * here on hands the words over rather than asking for them again. */
        char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
        bool by_number = false;
        bool resume = false;
        int outcome;
        for (;;) {
            outcome = restore_mnemonic(count, words, &by_number, mnemonic, sizeof(mnemonic), resume);
            resume = false;
            if (outcome != 1) {
                break;
            }
            if (!nav_acknowledge(
                    "Checksum valid", "BIP39 English", "Derivation unlocked", "Open wallet", false)) {
                /* Back off the verdict is the review again, with a restore
                 * that already succeeded still in hand. */
                resume = true;
                continue;
            }
            show_wallet_data(mnemonic);
            break;
            /* Back off the wallet's first screen: the verdict again. */
        }
        seedtool_zero(words, sizeof(words));
        seedtool_zero(mnemonic, sizeof(mnemonic));
        if (outcome != 0) {
            return;
        }
    }
}

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
    size_t cursor = 0;
    for (;;) {
        char orientation_item[32], brightness_fraction[8], brightness_item[24];
        (void)snprintf(orientation_item, sizeof(orientation_item), "Flip Orientation: %s",
            orientation_flipped ? "On" : "Off");
        format_brightness(brightness_fraction, sizeof(brightness_fraction));
        (void)snprintf(brightness_item, sizeof(brightness_item), "Brightness: %s", brightness_fraction);
        const char* items[] = { orientation_item, brightness_item, "About" };
        const int selected = choose_menu_at("Settings", items, 3, &cursor);
        if (selected < 0 || selected == 3) {
            return;
        }
        if (selected == 0) {
            orientation_flipped = !orientation_flipped;
            seedtool_display_set_orientation(orientation_flipped);
        } else if (selected == 1) {
            show_brightness();
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
        if (selected < 0) {
            seedtool_platform_restart();
        } else if (selected == 0) {
            show_new_seed_menu();
        } else if (selected == 1) {
            restore_seed();
        } else {
            show_settings_menu();
        }
        last_action = seedtool_platform_milliseconds();
    }
}
