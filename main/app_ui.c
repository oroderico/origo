/* The app's screen machinery: key input and the session timeout, the nav
 * chrome's lists and confirms, paged text, and the value, coin and card
 * entry screens every flow builds on. */
#include "app_internal.h"

#include <stdio.h>
#include <string.h>

uint64_t last_action;
bool chord_learned;
bool orientation_flipped;
unsigned backlight_level = SEEDTOOL_DISPLAY_BRIGHTNESS_DEFAULT;

void seedtool_require(const bool condition)
{
    if (!condition) {
        seedtool_platform_restart();
    }
}

/* Shows what a seedtool_render_qr* call drew. Nothing is drawn, and nothing
 * shown, when the payload does not fit. */
bool show_qr(const bool drawn)
{
    if (drawn) {
        seedtool_display_flush();
    }
    return drawn;
}

void screen_text(const char* title, const char* line1, const char* line2, const char* footer)
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
seedtool_key_t wait_key(void)
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
seedtool_key_t wait_key_or_tick(const uint32_t frame_ms, bool* const ticked)
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
 * confirm. `label` NULL leaves out the confirm; `progress` adds the entropy
 * bar. Returns NAV_BACK or NAV_CONFIRM. */
int nav_screen(const char* title, const char* one, const char* two, const char* label,
    const bool start_on_back, const seedtool_progress_t* progress)
{
    seedtool_nav_t nav = {
        /* A screen with no confirm opens on the arrow, whatever the caller
         * asked. */
        .selected = start_on_back || !label ? SEEDTOOL_NAV_BACK : SEEDTOOL_NAV_CONFIRM,
        .confirm = label ? SEEDTOOL_CONFIRM_TICK : SEEDTOOL_CONFIRM_NONE,
        .confirm_enabled = true,
        .back = true,
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
            if (label) {
                nav.selected = nav.selected == SEEDTOOL_NAV_BACK ? SEEDTOOL_NAV_CONFIRM : SEEDTOOL_NAV_BACK;
            }
            break;
        default:
            break;
        }
    }
}

/* A screen the reader takes or leaves. `start_on_back` is the opt-out from
 * opening on the bar, for a screen whose whole purpose is to say "this may be
 * a bad idea". */
bool nav_acknowledge(
    const char* title, const char* one, const char* two, const char* label, const bool start_on_back)
{
    return nav_screen(title, one, two, label, start_on_back, NULL) == NAV_CONFIRM;
}

/* The same with the entropy quality bar drawn in. */
bool nav_dice_confirm(const char* title, const char* one, const char* two, const char* label,
    const bool start_on_back, const seedtool_progress_t* progress)
{
    return nav_screen(title, one, two, label, start_on_back, progress) == NAV_CONFIRM;
}

/* A notice: the reader reads it and leaves, so the arrow is its only control. */
void notice(const char* title, const char* one, const char* two)
{
    (void)nav_screen(title, one, two, NULL, true, NULL);
}

/* A list under the nav chrome. `cursor` carries the selection in and out, so a
 * screen reopened after an edit lands where it was; seed it with
 * SEEDTOOL_NAV_CONFIRM, SEEDTOOL_NAV_BACK or an item index.
 *
 * `back` is false only for the Origo menu, which has no level above it, and the
 * wallet, whose exit erases the session and so is a row rather than a one-press
 * arrow. */
int choose_nav(const char* title, const char* const* items, const size_t count, const char* confirm,
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
        default:
            break;
        }
    }
}

/* A paged screen's cursor: 0 is the arrow, 1..pages the pages and pages + 1 the
 * confirm, so moving down reads forward. Without `confirmable` the ring is the
 * arrow and the pages. */
size_t page_step(const size_t position, const size_t pages, const bool forward, const bool confirmable)
{
    const size_t ring = pages + (confirmable ? 2 : 1);
    return (position + (forward ? 1 : ring - 1)) % ring;
}

/* The page the body shows for a cursor position: the arrow reads with the
 * first page behind it, the bar with the last. */
size_t page_shown(const size_t position, const size_t pages)
{
    return !position ? 0 : position > pages ? pages - 1 : position - 1;
}

/* One key on a paged screen. The chord on the arrow or the confirm answers
 * NAV_BACK or NAV_CONFIRM; anything else moves `*position`, the chord on a page
 * reading on, and answers NAV_STAY. */
int page_key(size_t* const position, const size_t pages, const bool confirmable)
{
    switch (wait_key()) {
    case KEY_SELECT:
        if (!*position) {
            return NAV_BACK;
        }
        if (*position > pages) {
            return NAV_CONFIRM;
        }
        *position = page_step(*position, pages, true, confirmable);
        break;
    case KEY_NEXT:
        *position = page_step(*position, pages, true, confirmable);
        break;
    case KEY_PREV:
        *position = page_step(*position, pages, false, confirmable);
        break;
    default:
        break;
    }
    return NAV_STAY;
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
seedtool_nav_t page_nav(const size_t position, const size_t pages, const char* counter,
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

/* A menu: its rows are its actions, so it has no confirm. Returns a row index
 * or NAV_BACK. */
int choose_menu_at(const char* title, const char* const* items, const size_t count, size_t* const cursor)
{
    return choose_nav(title, items, count, NULL, false, true, cursor);
}

int choose_menu(const char* title, const char* const* items, const size_t count)
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

/* Entry `position` of `total` in a run, on a numeric carousel. `allowed` is
 * optional and indexed from `min`; disallowed values (cards already drawn) are
 * skipped. Returns true with `*value` set, or false when the reader took
 * `[back]`, a stop on the ring since both keys change the value. */
bool enter_value(const char* title, const unsigned position, const unsigned total, const unsigned min,
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
        format_progress_heading(heading, sizeof(heading), title, position, total, progress);
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
        } else {
            seedtool_render_dice_screen(
                heading, on_back ? "[back]" : shown, history, chord_learned ? NULL : NAV_FOOTER, progress);
        }
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            if (on_back) {
                return false;
            }
            *value = current;
            return true;
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
        default:
            break;
        }
    }
}

/* One coin flip: KEY_PREV ("up") is heads, KEY_NEXT tails, and the chord undoes
 * the last flip. One press per flip, for runs of 128 or 256. Returns true with
 * `*bit` set, or false to step back. */
bool enter_coin_flip(const char* title, const unsigned position, const unsigned total, unsigned* bit,
    const char* history, const seedtool_progress_t* progress)
{
    char heading[48];
    format_progress_heading(heading, sizeof(heading), title, position, total, progress);
    for (;;) {
        seedtool_render_dice_screen(heading, "Heads (up)   Tails (down)", history, NULL, progress);
        seedtool_display_flush();
        switch (wait_key()) {
        case KEY_SELECT:
            return false;
        case KEY_PREV:
            *bit = 1;
            return true;
        case KEY_NEXT:
            *bit = 0;
            return true;
        default:
            break;
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
bool enter_card(const unsigned position, const unsigned total, unsigned* value, const bool* available,
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
        if (!enter_value("Suit", position, total, 0, 3, &suit, format_suit, suit_allowed, history, progress)) {
            return false;
        }

        bool rank_allowed[13];
        for (unsigned r = 0; r < 13; ++r) {
            rank_allowed[r] = !available || available[suit * 13 + r];
        }
        unsigned rank = 0;
        if (enter_value(
                CARD_SUIT_NAMES[suit], position, total, 0, 12, &rank, format_rank, rank_allowed, history, progress)) {
            *value = suit * 13 + rank;
            return true;
        }
    }
}

void hexstr(const uint8_t* bytes, const size_t len, char* output)
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
bool page_text_impl(const char* title, const char* text, const bool confirmable, const bool grouped)
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
    int answer = NAV_STAY;
    /* Outside the loop so the one exit can wipe them: they hold whatever is
     * paged, the mnemonic included. */
    char line1[MAX_LINE_CHARS + 1], line2[MAX_LINE_CHARS + 1], line3[MAX_LINE_CHARS + 1];
    while (answer == NAV_STAY) {
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
        answer = page_key(&position, pages, confirmable);
    }
    seedtool_zero(line1, sizeof(line1));
    seedtool_zero(line2, sizeof(line2));
    seedtool_zero(line3, sizeof(line3));
    return answer == NAV_CONFIRM;
}

/* Paged text with something to take at the end of it - the descriptor, the
 * account key, the transcript and its hash, the completed mnemonic. The
 * caller acts on the answer, so the tick is there to give one. */
bool page_text(const char* title, const char* text)
{
    return page_text_impl(title, text, true, false);
}


/* Paged text with nothing to take: read, then leave by the arrow. */
void page_read(const char* title, const char* text)
{
    (void)page_text_impl(title, text, false, false);
}

size_t step_key(const bool* enabled, const size_t count, size_t index, const bool forward)
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
size_t nearest_enabled(const bool* enabled, const size_t count, const size_t index)
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
size_t layout_key_index(const char* layout, const char target)
{
    const size_t count = seedtool_layout_keys(layout);
    for (size_t i = 0; i < count; ++i) {
        if (seedtool_layout_key(layout, i) == target) {
            return i;
        }
    }
    return count;
}
