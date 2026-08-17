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
/* Minimum roll counts hardly ever land exactly on the bit minimum; this keeps
 * that expected shortfall from popping the entropy warning on its own. Krux's
 * ENTROPY_TOLERANCE. */
/* Cushion added on top of the Miller-Madow bias correction (see
 * seedtool_dice_entropy_bias_bits), which does most of the work of keeping a
 * genuinely random run from reading as "poor". */
#define DICE_ENTROPY_TOLERANCE 4
#define SPLASH_MS 2500
#define MAX_PAGE_LINES 24
#define MAX_LINE_CHARS 48
#define PASSPHRASE_TAIL 24

/* Every index up to SEEDTOOL_MAX_ADDRESS_INDEX is derived and cached once per
 * visit to the Addresses screen, but only the first ADDRESS_SHOWN_ROWS get a
 * row in the list - scrolling to index 87 five rows at a time is its own
 * problem, one "Go to index" solves directly instead. */
#define ADDRESS_LIST_ROWS (SEEDTOOL_MAX_ADDRESS_INDEX + 1)
#define ADDRESS_SHOWN_ROWS 50
#define ADDRESS_LABEL_LEN (8 + SEEDTOOL_MAX_ADDRESS_LEN)
/* derive_addresses labels one shown row per derived address, so showing more
 * rows than were derived would read past the end of `addresses`. */
_Static_assert(ADDRESS_SHOWN_ROWS <= ADDRESS_LIST_ROWS, "more address rows are shown than are derived");

/* "Up/Down" rather than "L/R": the two buttons sit one above the other on
 * the board's left edge, not side by side, so the left button (KEY_PREV)
 * reads physically as "up" and the right (KEY_NEXT) as "down" - see the
 * numeric carousel's own reasoning in enter_value, which already treats
 * left as "up" for exactly this reason. Spelled out rather than an arrow
 * glyph: the fonts are 95-character ASCII with no arrow, and there is no
 * font-generation tool in this repo to add one - a hand-drawn icon (the
 * backspace key's own fix for the same gap) would mean the footer stops
 * being plain text, which every screen that draws one currently assumes. */
#define NAV_FOOTER "Up/Down move   BOTH select"
/* The scrolled digit field keeps a footer: up and down are the digit there,
 * not a cursor, so the nav chrome's two controls have nothing to attach to
 * and the hint is still the only thing that says what the buttons do. */
#define DIGIT_FOOTER "Up/Down set   BOTH take"

/* Word entry keyboard: the letters plus backspace. */
#define WORD_LAYOUT SEEDTOOL_WORD_LAYOUT
#define WORD_KEYS (SEEDTOOL_LETTERS + 1)

/* Word number keyboard, for a backup that records numbers rather than words. */
#define WORD_NUMBER_LAYOUT SEEDTOOL_WORD_NUMBER_LAYOUT
#define WORD_NUMBER_KEYS (SEEDTOOL_DIGITS + 2)

/* Same physical keypad as WORD_NUMBER_LAYOUT, reused for the account index:
 * exactly enough digits for SEEDTOOL_MAX_ACCOUNT_INDEX (999), so every digit
 * key stays reachable at any prefix length up to the cap - no need for the
 * word-number keyboard's variable reachable-digit pruning, which exists only
 * because 2048 isn't a round power of ten. */
#define ACCOUNT_DIGITS 3

/* Same reasoning as ACCOUNT_DIGITS, sized for SEEDTOOL_MAX_ADDRESS_INDEX (99)
 * instead: "Go to index" on the address list. The two constants are only
 * compatible by arithmetic nobody restates when editing one of them - two
 * digits reach exactly 99 - so changing either alone is a build error rather
 * than a keypad that can type an index the derived set does not hold.
 * browse_addresses rejects an out-of-range index at runtime as well: this
 * pins the pair, that refuses to act on the gap if the pin is ever loosened. */
#define ADDRESS_INDEX_DIGITS 2
_Static_assert(ADDRESS_INDEX_DIGITS == 2 && SEEDTOOL_MAX_ADDRESS_INDEX == 99,
    "the address-index keypad width and the derived address range must be changed together");

/* The word-number field draws one box per digit, and the renderer bounds how
 * many of those fit across the display. Widening a word number without the
 * boxes to show it would run the row off both edges, which clips silently. */
_Static_assert(SEEDTOOL_MAX_WORD_DIGITS <= SEEDTOOL_DIGIT_BOXES_MAX,
    "a word number has more digits than the digit field can draw boxes for");

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

/* Session-only display settings: like chord_learned, these live only in RAM
 * and reset to their defaults (unflipped, SEEDTOOL_DISPLAY_BRIGHTNESS_DEFAULT)
 * every boot - Origo has no persistence to save them to. */
static bool orientation_flipped;
static unsigned backlight_level = SEEDTOOL_DISPLAY_BRIGHTNESS_DEFAULT;

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

/* A flipped orientation means the case, and the buttons wired to it, are
 * physically rotated 180 degrees too - so what used to read as the left
 * button is now on the user's right. Swapping PREV/NEXT here, the one place
 * every raw key passes through, keeps "left" and "right" matching what is
 * shown on screen everywhere: menus, numeric entry, the keyboard. */
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
    screen_text("Session timeout", "Secrets will be erased", "in 60 seconds", "BOTH extend   Up/Down erase");
    /* Erasing is one of the two answers this screen accepts, and the user cannot
     * have meant it with a press begun before the screen existed. */
    seedtool_platform_flush_keys();
    /* The warning has replaced the caller's screen, so an extended session must
     * repaint it rather than let the next press act on what is no longer shown. */
    return wait_key_raw(WARNING_TIMEOUT_MS) == KEY_SELECT ? KEY_REDRAW : KEY_TIMEOUT;
}

/* Same idle/session-timeout accounting as wait_key, but for a screen that
 * redraws itself on a timer rather than only on a press: returns KEY_TIMEOUT
 * (with *ticked set) every frame_ms with no key down, instead of only after
 * the whole session-timeout warning dance. *ticked is left false for every
 * other return, including the genuine KEY_TIMEOUT the warning screen itself
 * produces when its extension offer is declined -- that one still means
 * leave, exactly as it does for wait_key's callers. */
static seedtool_key_t wait_key_or_tick(const uint32_t frame_ms, bool* const ticked)
{
    *ticked = false;
    const uint64_t elapsed = seedtool_platform_milliseconds() - last_action;
    const uint32_t idle_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    if (idle_ms < SESSION_TIMEOUT_MS) {
        const uint32_t remaining = SESSION_TIMEOUT_MS - idle_ms;
        const uint32_t wait_ms = frame_ms < remaining ? frame_ms : remaining;
        const seedtool_key_t key = wait_key_raw(wait_ms);
        if (key == KEY_TIMEOUT && wait_ms == frame_ms) {
            *ticked = true;
        }
        return key;
    }
    screen_text("Session timeout", "Secrets will be erased", "in 60 seconds", "BOTH extend   Up/Down erase");
    seedtool_platform_flush_keys();
    return wait_key_raw(WARNING_TIMEOUT_MS) == KEY_SELECT ? KEY_REDRAW : KEY_TIMEOUT;
}


/* Outcomes of choose_nav, alongside a non-negative item index. Three ways out
 * rather than the two a plain index-or-error would carry: the reader left
 * through the arrow, the reader took the confirm bar, or nobody was there. */
#define NAV_TIMEOUT (-1)
#define NAV_BACK (-2)
#define NAV_CONFIRM (-3)

/* The nav chrome's ring, laid out the way the screen is: the back arrow at the
 * top, then the items, then the confirm bar at the bottom. Moving down off the
 * last one wraps to the first, as every list here already does. */
/* The ring the cursor walks: the arrow, then the items, then the confirm bar,
 * with either end absent on a screen that does not offer it. `back` is false
 * only for the two screens with no level above them, which is why the arrow's
 * slot is counted rather than assumed. */
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

/* The one loop behind every screen whose only controls are the arrow and the
 * bar. `progress` draws the entropy quality bar in and is NULL otherwise;
 * `back` false removes the arrow, leaving a notice the reader can only take
 * in. Returns NAV_BACK, NAV_CONFIRM or NAV_TIMEOUT.
 *
 * These were three near-identical loops - one for text, one for the dice
 * screens, one for the notices - differing only in which renderer they called
 * and which of the two controls existed. The differences are arguments now,
 * so a fourth kind of screen is an argument too rather than a fourth copy of
 * the same switch. */
static int nav_screen(const char* title, const char* one, const char* two, const char* label, const bool back,
    const bool start_on_back, const seedtool_progress_t* progress)
{
    seedtool_nav_t nav = {
        /* Opening on a control that is not drawn would leave nothing lit and
         * nowhere to move to, since the ring below only turns when both
         * controls exist. No label means no tick, so such a screen opens on
         * the arrow whatever the caller asked for - the condition is derived
         * here rather than left as an unwritten rule the callers must keep. */
        .selected = back && (start_on_back || !label) ? SEEDTOOL_NAV_BACK : SEEDTOOL_NAV_CONFIRM,
        .confirm = label,
        .confirm_enabled = true,
        .back = back,
        /* A screen the reader takes or leaves has two controls, and one the
         * reader can only take in has just the arrow; either way they sit
         * where Jade puts them: leave on the left of the title bar, take on
         * the right, nothing along the bottom. The label stays in the struct
         * for the width checks even though no bar draws it - the title is what
         * tells the reader what taking it does, on every screen here but the
         * two entropy verdicts, which pass no title and say it in the body
         * instead for the reason given at their call. */
        .confirm_style = SEEDTOOL_CONFIRM_TICK,
    };
    for (;;) {
        if (progress) {
            seedtool_display_nav_dice(&nav, title, one, two, progress);
        } else {
            seedtool_display_nav_text(&nav, title, one, two, NULL);
        }
        switch (wait_key()) {
        case KEY_SELECT:
            return nav.selected == SEEDTOOL_NAV_BACK ? NAV_BACK : NAV_CONFIRM;
        case KEY_PREV:
        case KEY_NEXT:
            /* Two controls at most, so either direction is the other one - and
             * with only one of them there is nothing to move to. Both halves of
             * that guard are load-bearing: a screen with no arrow has nowhere
             * to go from the tick, and a screen with no label has no tick, so
             * toggling would park the cursor on a control that is not drawn and
             * leave nothing lit for the reader to aim at. */
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

/* A notice the reader can only take in: the screens whose answer every caller
 * discards. They used to carry "BOTH continue   Up/Down back" over a screen
 * where back and continue did the same thing, so the footer was describing a
 * way out that was not there.
 *
 * The tick went the same way, and for the same reason. A tick is the answer to
 * a question - the firmware says so itself where a menu is drawn, which passes
 * no label because "a tick there offers an answer to a question the screen
 * never asked" - and a screen whose result every caller throws away asks
 * nothing. What the reader does here is read it and leave, so the only control
 * is the one that means leave. These carried the opposite pair until now: a
 * tick and no arrow, the single way out wearing the glyph for assent. */
static void notice(const char* title, const char* one, const char* two)
{
    (void)nav_screen(title, one, two, NULL, true, true, NULL);
}

/* A choice made under the back-arrow-and-confirm-bar chrome. `cursor` carries
 * the selection in and out so a caller that reopens the same screen after an
 * edit lands where it left off; seed it with SEEDTOOL_NAV_CONFIRM for the
 * default of starting on the confirm bar, or SEEDTOOL_NAV_BACK, or an item
 * index, for a screen that wants otherwise.
 *
 * No footer hint: the controls are on screen, and the bar is standing where
 * the hint used to be drawn. Any screen converted to this chrome is reached
 * well past the menus that teach the chord.
 *
 * `back` is false for the two screens with nowhere to go up to. The Origo menu
 * has no level above it, and the wallet's only exit erases the session and
 * reboots - putting that on the arrow would give a control that is cheap and
 * reversible everywhere else a destructive meaning here, one press from any
 * row. Both keep their exit as a row the reader has to travel to and take, and
 * wear the rest of the chrome. A screen with somewhere to go back to gets an
 * arrow; a screen without one does not pretend. */
static int choose_nav(const char* title, const char* const* items, const size_t count, const char* confirm,
    const bool confirm_enabled, const bool back, size_t* const cursor)
{
    size_t top = 0;
    /* A cursor left on a control this draw does not offer - the confirm bar
     * of a screen that has since become unconfirmable, an item index from a
     * longer list, or the arrow on a screen that has none - falls back to the
     * first item, which every list has. */
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
            .confirm = confirm,
            .confirm_enabled = confirm_enabled,
            .back = back,
            /* The tick here too, so there is one confirm control in the
             * firmware and it is always in the same corner. Reading the ring
             * as a walk down the screen breaks at exactly one point either
             * way: with a bar, moving down off it wrapped to the arrow at the
             * top; with the tick, the last row wraps to the corner instead. A
             * ring has one discontinuity wherever it is put, and putting it
             * here sets the two ways out of a screen side by side rather than
             * at opposite ends of the glass. */
            .confirm_style = SEEDTOOL_CONFIRM_TICK,
        };
        seedtool_display_nav_list(&nav, title, items, count, top);
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

/* Where the cursor sits on a paged screen: 0 is the back arrow, 1..pages are
 * the pages themselves, and pages + 1 is the confirm bar. Moving down is
 * therefore reading forward - the cursor *is* the reading position - and the
 * first page's neighbour above is the arrow rather than the way out.
 *
 * `confirmable` false drops that last stop, for a screen there is nothing to
 * agree to on. The ring is then the arrow and the pages, so reading past the
 * last page wraps to the arrow: down, down, and back out the way in. */
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

/* The chrome a paged screen wears, so page_text and show_numbered_list say it
 * once each rather than both spelling out the same four fields.
 *
 * `style` is theirs to choose because their confirms mean different things:
 * paged text hands off to another screen, a numbered list is agreed to. */
static seedtool_nav_t page_nav(const size_t position, const size_t pages, const char* counter,
    const bool confirmable, const seedtool_confirm_t style)
{
    const seedtool_nav_t nav = {
        .selected = page_selection(position, pages),
        /* No label is what removes the control, the same way a menu removes
         * it: the reading screens have nothing for the reader to agree to or
         * to go on to, so the corner is left empty rather than given a meaning
         * the caller then discards. */
        .confirm = confirmable ? "Continue" : NULL,
        .confirm_enabled = true,
        .back = true,
        .counter = counter,
        /* On a paged screen the ring is the reading order - arrow, page 1..N,
         * then the corner - so the confirm being a corner rather than a bar
         * changes where the cursor lands after the last page and nothing about
         * how the pages are read. */
        .confirm_style = style,
    };
    return nav;
}

/* A menu under the nav chrome: what used to be a Back row at the bottom of
 * the list is the arrow in the title bar instead, where every other screen
 * now keeps it. No confirm bar - a menu's rows are its actions, and there is
 * nothing left to confirm once one is picked.
 *
 * Taking the arrow returns `count`, the index the Back row itself used to
 * occupy, so a caller keeps the dispatch it already had: drop "Back" from the
 * array and the checks against its old index still read the same answer.
 * `cursor` opens on the first option rather than on the arrow - a menu has no
 * confirm for the usual default to land on, and the options are the point. */
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

/* The "N/M" position counter every entry screen's title carries, with the
 * bits collected so far appended when `progress` says it has them (`graded`
 * is false on the default-initialised structs elsewhere in this file that
 * only draw a bar, and `progress` is NULL outright on the
 * checksum-completion flips, which are not graded at all - see
 * seedtool_progress_t's doc comment). Kept to one helper so the
 * dice/card and coin-flip screens can't drift into two different formats
 * for the same thing.
 *
 * The running count alone, not "bits of minimum": the minimum is already
 * said three times over - the opening screen names it, the bar's lower
 * segment fills against it, and the end-of-run screen spells out "X of Y
 * bits" - and repeating it here cost the width that matters. A coin's
 * counters are the worst of both: one flip is one bit, so "256/256
 * 257/256 bits" reads as a typo of itself and left the title 5px from the
 * glass, where the count alone leaves 27px.
 *
 * "bits" is spelled out rather than abbreviated to a "b" the reader has to
 * decode next to three other numbers; the four characters that costs are
 * why collect_entropy() labels the coin screen "Coins" instead of the
 * menu's "Coin flips". */
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
 * values are skipped, which is how already-drawn cards are kept out of reach.
 * A `total` of zero means this is a one-off value rather than one of a run.
 *
 * Returns 1 when a value was chosen, 0 when the user stepped onto `[back]`, and
 * -1 on timeout — the same contract as enter_word(). Left and right are spending
 * their two gestures on the value itself, so backing out has to be a position in
 * the ring rather than a key, exactly as `[delete]` is in the word list. Without
 * it a mistake on roll 29 of 60 could only be escaped by waiting out the session
 * timeout and starting the whole transcript again. */
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
        /* The running transcript sits under the value being entered, so a
         * mis-keyed roll is caught against the paper now rather than at the end
         * of ninety-nine of them. Only its tail fits, which is the part that
         * just changed. */
        /* A plain number scrolls in its own box, the same shape the word-number
         * field uses - up and down are the value, and there is nothing else on
         * screen to hunt through. A formatted value (a card, "A Clubs") is too
         * wide for a box and stays on the text screen, where the running
         * transcript sits under it. */
        if (!format) {
            seedtool_display_value_box(
                heading, shown, on_back, chord_learned ? NULL : NAV_FOOTER, progress);
        } else if (progress) {
            seedtool_display_dice_screen(
                heading, on_back ? "[back]" : shown, history, chord_learned ? NULL : NAV_FOOTER, progress);
        } else {
            screen_text(heading, on_back ? "[back]" : shown, history, chord_learned ? NULL : NAV_FOOTER);
        }
        switch (wait_key()) {
        case KEY_SELECT:
            if (on_back) {
                return 0;
            }
            *value = current;
            return 1;
        /* A value is a quantity, not a position in a list: unlike every list or
         * keyboard screen, where KEY_PREV/KEY_NEXT move toward the previous or
         * next item, here KEY_PREV raises the value and KEY_NEXT lowers it —
         * matching the physical button read as "up" on this board being the
         * one that increases what is on screen, the same way a spinner's up
         * arrow does, rather than reusing list-navigation's sense of "previous". */
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

/* A coin flip is a choice between two outcomes, not a position in a range: one
 * press per flip (KEY_PREV, physical "up", for Heads; KEY_NEXT for Tails)
 * rather than cycling a carousel to the wanted value and confirming it —
 * worth doing here specifically because a run is 128 or 256 flips long, so
 * halving the presses per flip halves the whole entry. There is no longer a
 * neutral carousel position to escape through, so KEY_SELECT (both buttons)
 * takes over as "undo the last flip", the same contract enter_value's [back]
 * had: 0 for the caller to step back one, 1 with `*bit` set for a flip made,
 * -1 on timeout.
 *
 * The prompt no longer needs a whole body line for a value the reader picks
 * directly, so that line goes to a second line of transcript tail instead —
 * with no carousel to spend a keypress paging through it, showing passively
 * more of what was just flipped is pure upside.
 *
 * No footer hint: NAV_FOOTER's "Up/Down move BOTH select" would be wrong here —
 * up and down commit a choice rather than moving one, and BOTH undoes rather
 * than selects — and every path that reaches this screen already went through
 * at least one chord-gated menu first, so chord_learned is always true by then
 * anyway. */
static int enter_coin_flip(const char* title, const unsigned position, const unsigned total, unsigned* bit,
    const char* history, const seedtool_progress_t* progress)
{
    char heading[48];
    format_progress_heading(heading, sizeof(heading), title, position, total, progress);
    for (;;) {
        seedtool_display_dice_screen(heading, "Heads (up)   Tails (down)", history, NULL, progress);
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

/* A card is picked in two carousels, suit then rank within it, rather than
 * one 52-way ring: scanning the whole deck from `AC` every time averages
 * about 26 presses per card across the 25 a seed needs, where 4-way then
 * 13-way averages about 8.5. Backing out of the rank step returns to the
 * suit step - one stage, matching every other back gesture in the app -
 * and only backing out of the suit step undoes the card itself, which the
 * caller already handles exactly as it did for the single-carousel pick
 * this replaces (same 1/back/timeout contract as enter_value). */
/* `available`, like `enter_value`'s own `allowed`, is optional: NULL means
 * every suit and rank is reachable, the with-replacement draw's case, where
 * nothing is ever excluded. */
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

/* Three body lines per page, split by what actually fits on the display
 * rather than by a character count. Three rather than two costs nothing but
 * the tighter line pitch the renderer uses for a third line: a third fewer
 * pages to review the same value. Returns true when the reader advanced past
 * the last page or accepted, false when they backed out. */
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
    /* Hoisted out of the loop so the single exit below can wipe them: what
     * these three hold is whatever the caller is paging, and the callers page
     * the canonical transcript, its SHA256 and the mnemonic itself. `start`
     * and `length` are offsets into the caller's own string and carry nothing
     * on their own. */
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
            /* Where each line sits in the whole value, in groups, so the
             * alternating ink carries across the line break rather than
             * restarting three times a page. Every line but a final short one
             * is a whole number of groups, which is what makes this exact. */
            const char* const shown[] = { line1, line2, line3 };
            const size_t first_group[] = { start[first] / SEEDTOOL_GROUP_LEN,
                (first + 1 < lines ? start[first + 1] : 0) / SEEDTOOL_GROUP_LEN,
                (first + 2 < lines ? start[first + 2] : 0) / SEEDTOOL_GROUP_LEN };
            seedtool_display_nav_grouped(&nav, title, shown, first_group, 3);
        } else {
            seedtool_display_nav_text(&nav, title, line1, line2, line3);
        }
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
            /* The chord on a page reads on rather than doing nothing: it is
             * the same direction the old one went, one step at a time now
             * instead of straight out of the screen. */
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


/* Paged text there is nothing to take. Returns nothing because there is
 * nothing to return: the reader pages down and leaves by the arrow, and a
 * caller that cannot ask for an answer cannot forget to use it. */
static void page_read(const char* title, const char* text)
{
    (void)page_text_impl(title, text, false, false);
}

/* The account key carries its key origin, so a scan does not have to be told
 * the derivation path afterwards. The account component's "999" is
 * SEEDTOOL_MAX_ACCOUNT_INDEX's own width, not a placeholder. */
#define ACCOUNT_KEY_LEN (sizeof("[00000000/84'/0'/999']") + SEEDTOOL_MAX_XPUB_LEN)

/* A BIP380 output descriptor for this account, checksum included: prefix,
 * bracketed origin, xpub, BIP389's "<0;1>" multipath chain step and wildcard
 * "*" suffix, a "#" and 8 checksum characters. "wpkh" rather than "tr" since
 * it is one character longer, the only other prefix this ever produces, so it
 * is the binding case; the account component and the checksum's fixed 8
 * characters follow ACCOUNT_KEY_LEN's own convention above. */
#define DESCRIPTOR_LEN (sizeof("wpkh([00000000/84'/0'/999']/<0;1>/*)#12345678") + SEEDTOOL_MAX_XPUB_LEN)

/* The upper bound on a single BBQr part's byte length: a part is never
 * longer than the whole value it is cut from, and a descriptor - longer than
 * the bracketed account key alone - is now the largest value this firmware
 * ever hands to show_bbqr, not always the account key. */
#define BBQR_MAX_VALUE_LEN ((ACCOUNT_KEY_LEN > DESCRIPTOR_LEN ? ACCOUNT_KEY_LEN : DESCRIPTOR_LEN) - 1)
#define BBQR_FRAME_LEN (SEEDTOOL_BBQR_HEADER_LEN + (BBQR_MAX_VALUE_LEN * 8 + 4) / 5 + 1)

/* Frames auto-advance with no press needed, and that is the only way they
 * advance: up and down move between the screen's two controls instead, the
 * same as on every other QR. Stepping a multi-part code by hand was the wrong
 * shape anyway - a reader collects the parts across repeats, so a frame held
 * still can never complete one - and it was spending both keys on every
 * animated screen to do it.
 *
 * Slower than the 700ms it ran at while a reader could step out of the
 * animation, since now they cannot: a camera that misses a frame waits a whole
 * cycle for it to come round. A guess, like the last one - the number to beat
 * is whatever drops frames on a real panel. */
#define BBQR_FRAME_INTERVAL_MS 1000

/* Each frame is capped well below this file's QR_VERSION so it draws at a
 * coarser, more legible module grid on this display's 135px height: versions
 * 4-6 all land on the same 3px modules, version 3 steps up to 4px, and 1-2
 * to 5px but at roughly double the frame count (and cycling time) version 3
 * needs for the account key -- 3 is the more legible-for-fewer-frames
 * trade-off. */
#define BBQR_FRAME_MAX_VERSION 3

/* Steps an account key too big for one QR at BBQR_FRAME_MAX_VERSION through
 * BBQr (github.com/coinkite/BBQr) parts, the same animated multi-part
 * convention other hardware wallets read with a camera. Krux's own BBQr
 * support (src/krux/bbqr.py, src/krux/qr.py FORMAT_BBQR) is the reference
 * this follows for the wire format; there is no deflate library here to use
 * its "Z" compressed encoding, so this always sends "2" (plain base32).
 * Returns the key that ended the animation, exactly what a single
 * seedtool_display_qr call would have handed back. */
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
        /* export_qr's title buffer is 24 bytes; %.20s plus the widest
         * plausible "N/N" part counter still fits with room to spare, so
         * this never truncates in practice -- sized generously rather than
         * exactly so it stays provably within bounds regardless of what a
         * caller ever passes, which is what silences -Wformat-truncation. */
        char frame_title[48];
        if (parts > 1) {
            (void)snprintf(
                frame_title, sizeof(frame_title), "%.20s %u/%u", title, (unsigned)(part + 1), (unsigned)parts);
        } else {
            (void)snprintf(frame_title, sizeof(frame_title), "%.20s", title);
        }
        const bool ok = seedtool_bbqr_part((const uint8_t*)value, len, '2', 'U', part, parts, frame, sizeof(frame))
            && seedtool_display_qr(frame_title, frame, selected);
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

/* A single account key's own QR: no carousel to switch to some other value,
 * since the account key stands on its own here, but always shown through
 * show_bbqr's animation rather than a single still frame the way an address
 * (show_address_qr below) is -- the account key is long enough that a
 * static frame needs this file's full QR_VERSION, a fine enough module grid
 * on this display's 135px height to be worth the extra frames BBQR_FRAME_
 * MAX_VERSION's coarser ones cost. */
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

/* One BIP39 word. Returns 1 when a word was chosen, 0 when the user deleted
 * back out of this word, and -1 on timeout or overflow. Only letters that can
 * still lead to a word are reachable, and once ten or fewer words match the
 * remaining candidates are offered directly. The initial key and the candidate
 * order are deterministic; nothing here consults the RNG.
 *
 * `allowed` narrows which words count as words at all, NULL for the whole
 * list. Every enabled letter leads to at least one allowed word, so a stem
 * built here can never strand the reader with no candidates - the invariant
 * the empty-match bail below relies on. */
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
            /* Small candidate set: the remaining words are listed outright, with
             * the delete entry last. The list has taken over the area the stem
             * used to occupy, so the stem rides in the title instead. */
            const char* items[SEEDTOOL_MAX_WORD_CHOICES + 1];
            char listing[sizeof(title) + SEEDTOOL_MAX_WORD_LEN + 3];
            for (size_t i = 0; i < matches; ++i) {
                items[i] = seedtool_word(candidates[i]);
            }
            /* No [delete] row: the arrow is that control now. Deleting the
             * last letter of a stem is a step back, and on an empty stem it
             * always was one - it left the word entirely. The arrow returns
             * the index that row held, so the branch below is unchanged. */
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
                seedtool_display_keyboard(title, stem_len ? stem : "-", WORD_LAYOUT, enabled, selected, position, total);
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

/* One BIP39 word, entered as its one-based number instead of its letters, for a
 * backup that records numbers. Same contract as enter_word(): 1 when a word was
 * chosen, 0 when the user deleted back out of this word, -1 on timeout or
 * overflow. Only digits that still lead to a word number are reachable, and the
 * number is shown as its word before it is accepted: a digit misread off paper
 * is otherwise a different seed with no sign that anything went wrong.
 *
 * `allowed` narrows that further on a mnemonic's last word, where the checksum
 * leaves only a handful of words possible. It is the same set the letter
 * keyboard narrows by, and it has to be applied here too: a restore that
 * refuses a word by letter and accepts it by number is worse than one that does
 * neither, because the reader who used the number pad has been told nothing.
 *
 * Up to four boxes, scrolled one at a time, and a number ends where the reader
 * says it does: `240` is typed as three digits, not as `0240`. The ring for the
 * current box carries the digits that still lead somewhere, then OK once what
 * is typed is already a word number, then backspace - the same ring Jade's
 * index entry uses, where a short number also has to be able to stop early.
 *
 * A fixed four-box field was tried first and was wrong: it made 240 a dead end,
 * since no fourth digit takes 240x into 1..2048, so the reader who typed it
 * found a box with nothing in it but backspace. Ending the number early is not
 * a convenience here, it is what makes three-digit numbers reachable at all.
 *
 * Replaces a twelve-key keypad the reader had to walk a cursor across. Two
 * buttons that mean "the digit goes up" and "the digit goes down" need no
 * hunting, and the row of boxes shows how much is left to type without a
 * separate readout. */
static int enter_word_number(
    const size_t position, const size_t total, const seedtool_wordset_t* allowed, char* output, const size_t output_len)
{
    char digits[SEEDTOOL_MAX_WORD_DIGITS + 1] = { 0 };
    /* What each box shows, including the one being scrolled. */
    char shown[SEEDTOOL_MAX_WORD_DIGITS + 1] = { 0 };
    size_t at = 0;
    char title[24];
    (void)snprintf(title, sizeof(title), "Word %u/%u", (unsigned)position, (unsigned)total);

    /* The ring for the current box: the digits still reachable, then backspace.
     * Rebuilt whenever the box changes, since which digits are reachable
     * depends on the ones already set. */
    char ring[SEEDTOOL_DIGITS + 2];
    size_t ring_len = 0, on = 0;
    bool rebuild = true;

    for (;;) {
        if (rebuild) {
            bool reachable[SEEDTOOL_DIGITS] = { false };
            /* Narrowed by `allowed`, exactly as the letter keyboard is. The
             * ring is built per box, so a digit that cannot begin an allowed
             * word never appears to be scrolled to in the first place. */
            (void)seedtool_next_digits_in(allowed, digits, at, reachable);
            ring_len = 0;
            for (size_t d = 0; d < SEEDTOOL_DIGITS; ++d) {
                if (reachable[d]) {
                    ring[ring_len++] = (char)('0' + d);
                }
            }
            /* OK only once the digits already name a word - so it is absent on
             * an empty field and on a prefix like `20`, which is a real number
             * but is offered here as a step towards 200x rather than as 20
             * itself... which it also is. Both readings are live, which is
             * exactly why the reader has to say which one they meant. */
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
        seedtool_display_digits(title, shown, SEEDTOOL_MAX_WORD_DIGITS, at, DIGIT_FOOTER, NULL);

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

        /* Narrowed here too, not only where the ring is built. The ring should
         * already have made a disallowed number untypeable - but this is the
         * gate a number passes through to become a word, and a gate that
         * inherits its guarantee from the screen upstream is one bug away from
         * accepting what that screen was supposed to have refused. A number
         * outside the set reads as no word at all, which sends the reader back
         * to the field rather than into a confirmation they should never see. */
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
        /* Went back from the confirmation. A number that confirmed itself on a
         * fourth digit reopens that digit, so a wrong last one costs a press
         * rather than the whole number; one the reader ended with OK returns to
         * the field exactly as it was, with the OK still on the ring. */
        if (typed_last) {
            digits[--at] = '\0';
        }
        rebuild = true;
    }
}

/* The account index (m/type'/0'/account'), typed on the same keypad
 * enter_word_number uses. Every digit key is reachable up to ACCOUNT_DIGITS
 * typed (unlike the word-number keyboard, any prefix here is already a
 * complete, in-range value - 0 through 999), so there is nothing to prune:
 * only the digit cap and Accept/Backspace's own enabling need computing.
 * Returns 1 with `*value` set when a number was accepted, 0 when the reader
 * backed out of the first digit, -1 on timeout - the same three-way contract
 * every other entry screen here uses. */
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
        /* Same fix as enter_word_number's Accept/Backspace race: once every
         * digit key goes dark at the length cap, jump straight to Accept
         * rather than trusting nearest_enabled's ring distance to land there. */
        selected
            = !enabled[selected] && digits_len ? accept_index : nearest_enabled(enabled, WORD_NUMBER_KEYS, selected);
        bool picked = false;
        while (!picked) {
            seedtool_display_keyboard(
                "Account", digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected, 0, 0);
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

/* Same shape as enter_account(), for jumping straight to an address index
 * instead of scrolling the list to it: ADDRESS_INDEX_DIGITS is exactly wide
 * enough for SEEDTOOL_MAX_ADDRESS_INDEX, so every digit stays reachable and
 * there is nothing to prune. Returns 1 with `*value` set on Accept, 0 on
 * backing out of the first digit, -1 on timeout. */
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
            seedtool_display_keyboard(
                "Go to index", digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected, 0, 0);
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

/* words[0..count) joined with single spaces, the same layout seedtool_generate's
 * transcript-to-mnemonic conversion and every published BIP39 vector use.
 * Shared by enter_mnemonic_words (to ask what the last word could be),
 * enter_mnemonic (once, after entry) and review_and_confirm (again after every
 * edit), so every one of them is working from the exact string
 * seedtool_validate_mnemonic will see. */
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

/* Returns 1 when a whole mnemonic was entered, 0 when the user backed out of the
 * first word or the method menu, -1 on timeout or overflow. Fills `words` (a
 * caller-owned array so restore_mnemonic below can keep reviewing them after
 * entry finishes) and, if `by_number` is not NULL, reports which entry method
 * was used - re-entering a single word during review needs the same one. */
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
        /* The last word of a whole mnemonic carries the checksum, so most of
         * the wordlist cannot end it: 128 of 2048 can for 12 words, 8 for 24.
         * Narrowing to those here is the same verdict the review screen
         * already gives afterwards, moved to where it prevents the mistake
         * instead of reporting it - a reader restoring from paper finds the
         * wrong last word unreachable rather than typeable and then refused.
         *
         * Only for a mnemonic that is meant to be whole. complete_checksum
         * enters 11 or 23 words precisely because their last word has no
         * checksum in it yet, and narrowing there would be narrowing to a
         * checksum the mnemonic is not supposed to satisfy. */
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
    /* The set is 128 of 2048 words picked out by the eleven before them, so it
     * says something about those eleven; wiped like everything else derived
     * from them. */
    seedtool_zero(&allowed, sizeof(allowed));
    return outcome;
}


static char review_labels[24][32];
static const char* review_items[24];

/* Lets the reader jump straight to any already-entered word and fix it,
 * rather than losing the other 11 or 23 correct ones over a single mistake -
 * restore_seed used to just discard the whole entry and show "INVALID
 * CHECKSUM" with no way back in. A scrolling list (choose_nav, cursor
 * persisted across edits), the same widget and pattern the address list
 * already uses to pick one item out of several to inspect or act on -
 * coherence with the rest of the app mattered more here than the carousel's
 * one-item-at-a-time feel, since this is fundamentally "pick which of these
 * to fix," not "read through them in order." Continuing and going back are
 * not rows here but the nav chrome's own two controls - the confirm bar and
 * the back arrow - so the list holds words and nothing else, and neither way
 * out moves as the word count does. This is the first screen on that chrome;
 * choose_nav is meant to take the rest as they are tested.
 * Shown after every full entry, not only a failed one, so a word
 * can be double-checked before continuing at all. `words` is edited in
 * place; `mnemonic` is rejoined from it after every change so
 * seedtool_validate_mnemonic always grades the current state. Returns 1 once
 * Continue is chosen with a valid checksum, 0 on explicit Back, -1 on
 * timeout or overflow - the same three-way contract every other entry
 * screen here uses. */
static int review_and_confirm(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, const bool by_number,
    char* mnemonic, const size_t mnemonic_len)
{
    /* The confirm bar by default, as every nav screen opens: the reader who
     * has just typed twelve words is far more often done than not. */
    size_t cursor = SEEDTOOL_NAV_CONFIRM;
    int outcome;
    /* Cleared at both ends, exactly as derive_addresses does with
     * address_labels: these rows are the mnemonic in plain text, one word
     * each, and they live in .bss rather than on a stack frame that the next
     * screen would overwrite anyway. Leaving them behind kept a restored seed
     * readable in RAM for the rest of the session. Both ways out of the wallet
     * now reboot, and C startup zeroes .bss, so that particular leak would no
     * longer outlive the session - but this wipe is what makes the rows gone
     * while the session is still running, which the reboot cannot do. Clearing
     * on entry as well as exit keeps a 12-word restore from leaving rows 12..23
     * of a previous 24-word one on show. */
    seedtool_zero(review_labels, sizeof(review_labels));
    for (;;) {
        if (!join_words(words, count, mnemonic, mnemonic_len)) {
            outcome = -1;
            goto done;
        }
        const bool valid = seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK;
        for (size_t i = 0; i < count; ++i) {
            /* Precision on %s, not a bare conversion: words[i] is genuinely
             * bounded (SEEDTOOL_MAX_WORD_LEN), but GCC's format-truncation
             * analysis loses that bound through the words[][...] parameter
             * decay once inlined this deep - see the identical fix on
             * derive_addresses's snprintf. */
            (void)snprintf(review_labels[i], sizeof(review_labels[i]), "%02u. %.*s", (unsigned)(i + 1),
                (int)SEEDTOOL_MAX_WORD_LEN, words[i]);
            review_items[i] = review_labels[i];
        }
        /* Continue can only be taken once it would actually do something: an
         * invalid checksum can't be acted on. It stays drawn either way,
         * dimmed rather than gone, so the control the reader is heading for
         * does not appear and disappear under them as words are fixed - the
         * title carries the verdict, as it already did. */
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
        /* Re-entering the last word narrows to what can actually end these
         * words, exactly as first entry does. Only the last one: every earlier
         * word is free, and it is usually an earlier word that is wrong - the
         * checksum fails on whichever word was mistyped, not necessarily on
         * the one carrying it.
         *
         * Built from the current words each time, so fixing word 3 and then
         * word 12 offers the candidates for the corrected prefix rather than
         * the one entry started with. If it cannot be built the word is simply
         * entered unnarrowed: this screen exists to fix a mnemonic that is
         * already wrong, so it must never be the thing that blocks the fix. */
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

/* The 11 or 23 words of a checksum completion, shown again when the reader
 * steps back out of the coin flips. review_and_confirm's list without its
 * checksum gate: a prefix this long is missing the very word the flips are
 * about to name, so it can never validate on its own - Continue is always
 * available here and the title never carries a verdict.
 *
 * What this exists for is that the words survive. complete_checksum used to
 * answer a step back from the first flip by entering the mnemonic again from
 * scratch - an empty array and the method menu: eleven words typed and gone,
 * for one press. */
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
        /* NULL: every word here is a prefix word, so none of them is the one
         * the checksum narrows - that is the word the flips are about to
         * name, and it is not in this list. */
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

/* The words stage of a checksum completion: the list, with the entry screen
 * behind it. Returns 1 once the reader comes forward again, 0 only when they
 * step back past the entry screen too, and -1 on timeout.
 *
 * The two are chained here rather than at the call site so that back means
 * one screen at every point of it: the list's arrow reopens the entry screen,
 * and only the entry screen's own arrow reaches the length menu. Backing out
 * of the list used to drop straight there, two stages at once. */
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

/* enter_mnemonic_words, then review_and_confirm on the result: the two-step
 * restore_seed actually wants. Kept apart from complete_checksum's own path,
 * which enters a still-partial (11 or 23 word) mnemonic that could never pass
 * seedtool_validate_mnemonic yet - the checksum gate belongs only where the
 * mnemonic is meant to be whole and correct, which is why that flow reviews
 * through review_prefix instead.
 *
 * No checksum is announced here, and the absence is deliberate: there is
 * nothing left for it to announce. The last word of a whole mnemonic is
 * entered under seedtool_final_word_candidates, both keyboards narrow to that
 * set rather than merely displaying it, and stepping back clears the word so
 * the last one is always entered last against a filter rebuilt from the words
 * currently standing. What leaves enter_mnemonic_words with success has a
 * checksum that holds.
 *
 * A one-shot notice did stand here, from before that filter existed, and its
 * own commit said the check was being moved to where it prevents the mistake
 * instead of reporting it. Only the moving happened. Restoring it would mean
 * restoring a screen no input can reach - and the reader is not left without a
 * gate either way, since review_and_confirm still reads the checksum on every
 * redraw and keeps Continue untakeable while it fails. */
static int restore_mnemonic(const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number,
    char* mnemonic, const size_t mnemonic_len, bool resume)
{
    /* Entry and review chained the way revisit_prefix chains them, so the
     * review's arrow reopens the entry screen rather than dropping past it to
     * the length menu. `resume` starts on the review instead: for a caller
     * that has taken the reader forward and needs to hand the words back
     * without asking for them again. */
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
        seedtool_display_keyboard("BIP39 passphrase", tail, layout, NULL, selected, 0, 0);
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

/* The optional passphrase, set and changed from Derivation rather than asked
 * for on the way in. A session starts without one and says so on Derivation's
 * own row, so the reader who does not use a passphrase never crosses a screen
 * about it, and the reader who does sets it in the same place they set the
 * account and the type - all three being inputs to the same derivation.
 *
 * Returns true only when `passphrase` actually changed, so the caller knows to
 * re-derive the fingerprint it has cached. Back, a timeout and a mismatch all
 * leave the acting passphrase exactly as it was: dropping a session silently
 * to no passphrase would change every derived key without saying so, which is
 * the one outcome this screen must never produce by accident. */
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
    /* Opens on the arrow, like every other screen whose purpose is the
     * warning on it: what this shows reveals every address of the account,
     * past and future, to anyone who photographs it. That is the same kind of
     * claim Compact SeedQR makes, and it earns the same treatment - the way
     * forward is not preselected on a screen that exists to say wait. */
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

/* A BIP380 output descriptor for this account - fingerprint, path, xpub and
 * a checksum, all in the one string most modern watch-only wallets (Sparrow,
 * Bitcoin Core, others) can import directly, rather than a reader typing the
 * key origin and xpub in by hand from the plain "Account key" screen above.
 * Same "reveals every address" warning as export_qr, since it carries the
 * exact same xpub - a descriptor just states the script type inline instead
 * of leaving it implied by which menu the xpub came from. Always plain xpub,
 * never zpub: SLIP-132 version bytes are a wallet-display convention BIP380
 * itself has no notion of.
 *
 * The chain step is BIP389's multipath "<0;1>" rather than a bare "0", so the
 * one descriptor describes both the receive and the change branch. A wallet
 * imported from a receive-only descriptor has nowhere to send its change and
 * would need a second import to get one - the descriptor exists to be the
 * whole account in one string, and half an account is the one thing it must
 * not be. BIP380's own INPUT_CHARSET already contains "<", ";" and ">", so
 * seedtool_descriptor_checksum covers this unchanged. */
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

/* A single address's own QR, and the first thing opening an address shows: no
 * account key in this view to warn about or to carousel over to, since a photo
 * of one address on its own reveals nothing the address itself did not
 * already. Scanning is what a reader almost always came for, so the code, its
 * path and the address itself share one screen rather than two.
 *
 * Not every address fits beside its code: a taproot address is 62 characters
 * against the 48 the margin holds. seedtool_display_qr_address refuses those
 * rather than drawing as far as it reaches - an address cut off mid-value
 * looks exactly like one that ended there - and they fall back to the code
 * alone, then the paged text, which has the width for them. */
/* A phone that will not lock onto a white block often locks onto a dimmer one,
 * so this screen carries a second control beside the way out: the chord on it
 * cycles how bright the code's light half is drawn, and pressing it again
 * keeps going round. A control rather than a pair of keys because the QR
 * screens that animate - the account key's BBQr frames, the Compact SeedQR
 * carousel - already spend up and down on stepping, and a shade bound to those
 * keys could never follow the shade anywhere it is most needed.
 *
 * Up and down move between the two controls, the chord takes whichever is
 * highlighted: the same two sentences as every other screen on the device. */
static void show_address_qr(const char* title, const char* address)
{
    size_t selected = SEEDTOOL_NAV_BACK;
    for (;;) {
        if (seedtool_display_qr_address(title, address, selected)) {
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
        if (!seedtool_display_qr(title, address, SEEDTOOL_NAV_BACK)) {
            notice("Too long for a QR", title, "Read it as text instead");
            return;
        }
        /* The fallback keeps the chrome it always had - one way out, and the
         * text after it - since the value that lands here is the one with no
         * room left in the margin for a second control. */
        if (wait_key() == KEY_REDRAW) {
            continue;
        }
        (void)page_text_impl(title, address, false, true);
        return;
    }
}

/* Addresses and their list labels are derived once per visit to the Addresses
 * screen and cached here for the whole visit, not re-derived every time the
 * reader backs out of one address's QR back to the list - only leaving the
 * screen for good retires the cache (see the end of show_addresses).
 * Static: this does not belong on the stack, and the list widget needs every
 * shown row addressable up front, there is no windowed variant of it.
 *
 * The trade this makes deliberately: these rows now stay populated while a
 * single address's text and QR are on screen, where re-deriving per visit used
 * to leave them populated only while the list itself was up. Addresses are
 * public - anyone holding the account xpub derives the same hundred - so this
 * is a privacy surface rather than a seed one, and the alternative was the
 * whole PBKDF2 and account derivation again on every step back from a QR.
 * Total .bss actually falls: only the shown rows are labelled now. */
static char addresses[ADDRESS_LIST_ROWS][SEEDTOOL_MAX_ADDRESS_LEN];
static char address_labels[ADDRESS_SHOWN_ROWS][ADDRESS_LABEL_LEN];
static const char* address_items[ADDRESS_SHOWN_ROWS + 2]; /* + Go to index + Back */

/* Fills the cache above for one type/account/branch. Every index up to
 * SEEDTOOL_MAX_ADDRESS_INDEX is derived - the mnemonic-to-seed PBKDF2 and the
 * hardened account step are the expensive part, and Go to index needs any of
 * them - but only the first ADDRESS_SHOWN_ROWS get a list row. One branch's
 * worth at a time: switching between receive and change re-enters here rather
 * than caching both, which keeps this .bss the size it already was and costs
 * only the derivation a type or account change costs today. Returns false
 * (having already told the reader) on a derivation error. */
static bool derive_addresses(const char* mnemonic, const char* passphrase, const seedtool_address_type_t type,
    const uint32_t account, const seedtool_chain_t chain, const char* const prefix)
{
    /* Cleared on entry as well as on the way out, exactly as review_labels is
     * and for the same reason: these are .bss, so a failed derivation must not
     * leave the previous account's rows sitting here to be wiped by a caller
     * that this time never gets far enough to do it. */
    seedtool_zero(address_labels, sizeof(address_labels));
    screen_text(prefix, "Deriving addresses...", NULL, NULL);
    if (seedtool_mainnet_addresses(mnemonic, passphrase, type, account, chain, ADDRESS_LIST_ROWS, addresses)
        != SEEDTOOL_OK) {
        seedtool_zero(addresses, sizeof(addresses));
        notice("Error", "Could not derive addresses", NULL);
        return false;
    }
    for (uint32_t i = 0; i < ADDRESS_SHOWN_ROWS; ++i) {
        /* Precision on %s, not a bare conversion: addresses[i] is genuinely
         * bounded (SEEDTOOL_MAX_ADDRESS_LEN), but that bound doesn't survive
         * the array-to-pointer decay through this call for GCC's format-
         * truncation analysis to see once inlined three levels deep into
         * show_wallet_data - it falls back to assuming an unbounded string
         * and flags a truncation risk that can't actually happen. An
         * explicit precision equal to the same bound gives it a provable
         * limit instead of a suppression. */
        (void)snprintf(address_labels[i], ADDRESS_LABEL_LEN, "%3u  %.*s", (unsigned)i,
            (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1), addresses[i]);
        address_items[i] = address_labels[i];
    }
    address_items[ADDRESS_SHOWN_ROWS] = "Go to index";
    return true;
}

/* Lets the reader browse the cache derive_addresses filled. Returns the
 * chosen index, or -1 on Back or timeout. On a selection, the chosen address
 * is copied into address_out; nothing here re-derives. `cursor` is both where
 * the list opens and where it is left: a reader who opens address 10, views
 * its QR and comes straight back lands on 10 again rather than back at the
 * top of the list. Go to index only moves `cursor` when it lands inside the
 * shown rows - an index past ADDRESS_SHOWN_ROWS has no row of its own to
 * leave the cursor on. */
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
            /* The keypad cannot type a value this high while ADDRESS_INDEX_DIGITS
             * and SEEDTOOL_MAX_ADDRESS_INDEX agree, and a _Static_assert holds
             * them together - but `index` addresses a static array here rather
             * than going through seedtool_mainnet_address, which is where that
             * range is otherwise checked. Verified rather than inherited: back
             * to the list instead of off the end of the cache. */
            if (index > SEEDTOOL_MAX_ADDRESS_INDEX) {
                continue;
            }
            if (index < ADDRESS_SHOWN_ROWS) {
                *cursor = index;
            }
            /* Explicit precision for the same reason derive_addresses gives
             * its own %s one: index, unlike the loop variable there, carries
             * no compile-time bound GCC's format-truncation analysis can see
             * through browse_addresses inlined into its callers. */
            (void)snprintf(address_out, address_out_len, "%.*s", (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1),
                addresses[index]);
            return (int)index;
        }
        /* No cursor assignment here: choose_menu_at has already left it on
         * whatever was taken, and writing the arrow's own return value back
         * would park it past the end of the list. */
        if (selected >= 0 && selected < (int)ADDRESS_SHOWN_ROWS) {
            (void)snprintf(address_out, address_out_len, "%.*s", (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1),
                addresses[selected]);
        }
        return selected < 0 || selected == (int)ADDRESS_SHOWN_ROWS + 1 ? -1 : selected;
    }
}

/* Every script type the viewer derives, in the order they are offered. Adding
 * one is a row here and nothing else: the Derivation row, the Wallet row, the
 * type menu's own title and the chooser all read their name from this table
 * rather than each carrying its own conditional to be found and updated. */
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

/* Which script type to derive: a list naming every type on offer, opened on
 * the one in force. Not a value cycled in place by pressing the row - two
 * types cycle tolerably and more do not, and a cycle never shows what it is
 * cycling through, so a reader would have to press past their choice to find
 * out what else existed. Returns false, leaving `*type` alone, on Back or a
 * timeout. */
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

/* Everything that hands out the account's extended public key, gathered behind
 * one row. xpub, zpub and the descriptor are the same 78 bytes three ways -
 * the plain BIP32 serialisation, the same key with SLIP-132's version bytes,
 * and the same key again with its script type and derivation stated inline for
 * a wallet to import - so they belong together rather than as separate rows
 * that look unrelated while carrying identical risk. Each one reveals every
 * address of the account, and each says so before it is shown.
 *
 * SLIP-132 defines no taproot version prefix, so zpub is offered for BIP84
 * only; under BIP86 the row is absent rather than present and refusing. */
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
        /* `count`, not `count - 1`: choose_menu_at returns the arrow as the
         * index one past the last row - the slot the Back row used to occupy -
         * so `count - 1` is the last real item instead. Here that item is
         * "Descriptor", which made the two swap: picking Descriptor left the
         * menu, and taking the arrow fell through to the else below and opened
         * the descriptor. The other menus compare against a literal that
         * already equals count, which is why this was the only one left. */
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
    /* Which branch, asked the same way and in the same shape the account
     * key's xpub/zpub choice is asked: the list itself is identical either
     * way, so the question belongs before it rather than as a mode to toggle
     * inside it. */
    const char* const branches[] = { "Receive", "Change" };
    const int branch = choose_menu("Addresses", branches, 2);
    if (branch < 0 || branch == 2) {
        return;
    }
    const seedtool_chain_t chain = branch == 0 ? SEEDTOOL_RECEIVE : SEEDTOOL_CHANGE;
    /* The path every row on this screen shares, and the screen's title. A list
     * of a hundred addresses is the one place the reader stays long enough to
     * forget what they are looking at, and its rows carry only an index and an
     * address - so the type, the account and the branch live in the title
     * rather than being remembered from the menu that opened it. Each address's
     * own path is this same string plus its index, built from it rather than
     * formatted a second time, so the title cannot drift from the rows. */
    char prefix[24];
    (void)snprintf(
        prefix, sizeof(prefix), "m/%u'/0'/%u'/%u", (unsigned)type, (unsigned)account, (unsigned)chain);
    if (!derive_addresses(mnemonic, passphrase, type, account, chain, prefix)) {
        return;
    }
    /* Loops back to the address list itself after each address's QR, rather
     * than out to the menu that opened it: picking another address is the
     * common next step, not re-choosing "Addresses" again - and, since
     * derive_addresses ran once above rather than on every pass through this
     * loop, coming straight back from one address's QR no longer costs the
     * whole derivation again. Only backing out of the list (or a timeout)
     * leaves. `cursor` lives outside this loop so the list reopens wherever it
     * was left, rather than back at address 0 every time. */
    size_t cursor = 0;
    for (;;) {
        char address[SEEDTOOL_MAX_ADDRESS_LEN] = { 0 };
        const int index = browse_addresses(prefix, address, sizeof(address), &cursor);
        if (index < 0) {
            seedtool_zero(address, sizeof(address));
            break;
        }
        /* The prefix, a separator and an index. Sized for a ten-digit index
         * rather than the two SEEDTOOL_MAX_ADDRESS_INDEX actually allows:
         * browse_addresses bounds the value, but that bound does not survive
         * into this frame for the compiler's format-truncation analysis to
         * see, and a buffer wide enough for what it can prove beats silencing
         * what it cannot - the same trade the %.*s precisions in this file
         * make. */
        char path[sizeof(prefix) + 12];
        (void)snprintf(path, sizeof(path), "%s/%u", prefix, (unsigned)index);
        /* One screen: the code, its path, and the address itself beside it.
         * Opening an address used to land on the text and reach the QR through
         * it, which cost a step to the reader who came to scan - and left the
         * two halves of the same fact on separate screens. */
        show_address_qr(path, address);
        seedtool_zero(address, sizeof(address));
    }
    seedtool_zero(addresses, sizeof(addresses));
    seedtool_zero(address_labels, sizeof(address_labels));
    /* A derivation path, not a secret - it is on screen the whole time and in
     * the README. Wiped anyway so the wipe check stays mechanical: `prefix`
     * holds a mnemonic elsewhere in this file, and a rule with one documented
     * exception is a rule that grows a second one. */
    seedtool_zero(prefix, sizeof(prefix));
}

/* One word per screen, stepped the same way show_qr steps between values:
 * L/R moves, anything else (BOTH, timeout) leaves. A plate punched from this
 * already restores today with zero new code, via "Enter word numbers". */
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
    /* The same ring the paged screens walk, with the words where their pages
     * go: position 0 is the arrow and 1..count are the words, so stepping off
     * either end of the carousel lands on the way out rather than wrapping
     * straight past it. This screen used to wrap forever and leave only by the
     * chord, which meant the one control it had was the one control it never
     * drew. `confirmable` is false throughout - there is nothing to accept
     * here, only a backup to read off and punch. */
    size_t position = 1;
    for (;;) {
        const size_t selected = page_shown(position, count);
        char footer[16];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(selected + 1), (unsigned)count);
        const char* const word = seedtool_word(numbers[selected] - 1);
        const seedtool_nav_t nav = page_nav(position, count, NULL, false, SEEDTOOL_CONFIRM_TICK);
        if (layout == 0) {
            seedtool_display_stackbit_screen(&nav, "Stackbit 1248", numbers[selected], word, footer);
        } else {
            seedtool_display_stackbit_physical_screen(&nav, "Stackbit 1248", numbers[selected], word, footer);
        }
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

/* Every word, numbered the way a physical backup numbers them (the same
 * seedtool_mnemonic_word_numbers/seedtool_word lookup show_stackbit uses),
 * four to a page rather than page_text's plain reflow: a reader checking word
 * 7 against paper can go straight to page 2 instead of counting words through
 * wrapped lines. Four words a page divides both 12 and 24 evenly, for 3 or 6
 * pages. `show_words` picks the line format: "position  word" for a reader
 * transcribing onto a word-based backup, "position  number" for one
 * transcribing onto a numeric one (Stackbit 1248 digits, or any backup that
 * only ever records the 1-2048 dictionary number) - same list, same paging,
 * just the second column changes. */
/* Same true/false contract as page_text: true on Select or paging past the
 * last page, false on backing out at the first page or a timeout - so a
 * caller chaining this into a must-complete sequence (show_generated) can
 * short-circuit on it exactly like any other page_text step, while
 * show_backup_menu's own callers, which don't chain, just discard it. Those
 * are the two wrappers below, and `confirmable` is the difference: the chained
 * one ends on a tick that means "written down", the browsed one has no such
 * step to offer and so does not draw one. */
static bool show_numbered_list_impl(const char* mnemonic, const bool show_words, const bool confirmable)
{
    uint16_t numbers[24];
    size_t count = 0;
    if (seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &count) != SEEDTOOL_OK) {
        notice("Error", "Could not compute", "word numbers");
        return false;
    }
    const size_t pages = (count + 3) / 4;
    /* Opens on the first page, like page_text: these are words to be written
     * down, so the cursor starts where the reading does rather than on the
     * way out. `cursor`, not `position` - that name is taken below by the
     * word's own place in the mnemonic. */
    size_t cursor = 1;
    bool advanced;
    /* Hoisted for the same reason as page_text's: every exit already wiped
     * `numbers`, the dictionary positions, while leaving the words those
     * positions spell rendered in full underneath. */
    char lines[4][24] = { { 0 } };
    for (;;) {
        memset(lines, 0, sizeof(lines));
        const size_t page = page_shown(cursor, pages);
        const size_t first = page * 4;
        for (size_t i = 0; i < 4 && first + i < count; ++i) {
            const size_t position = first + i + 1;
            const uint16_t number = numbers[first + i];
            /* Position zero-padded to a fixed two-digit field, not space-
             * padded: this font's digits share one advance width (tabular
             * figures) but its space glyph is narrower, so only zero-padding
             * keeps the field's total width - and everything after it - fixed
             * from row to row. That, plus screen4's left-aligned rows, lines
             * every word up under the last instead of each row centring
             * around its own width and the column drifting line to line. */
            if (show_words) {
                (void)snprintf(lines[i], sizeof(lines[i]), "%02u. %s", (unsigned)position, seedtool_word(number - 1));
            } else {
                (void)snprintf(lines[i], sizeof(lines[i]), "%02u. %04u", (unsigned)position, (unsigned)number);
            }
        }
        /* Sized for the format's worst case rather than its real one: pages
         * tops out at six here, but `page` now comes from page_shown() rather
         * than from the loop, so the compiler can no longer see a bound and
         * assumes ten digits apiece - and -Wformat-truncation is an error in
         * the firmware build even though the host build lets it pass. */
        char footer[24];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
        const seedtool_nav_t nav = page_nav(cursor, pages, footer, confirmable, SEEDTOOL_CONFIRM_TICK);
        const char* const rows[] = { lines[0], lines[1], lines[2], lines[3] };
        seedtool_display_nav_rows(&nav, show_words ? "BIP39 words" : "BIP39 word numbers", rows, 4);
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

/* Entering this screen reaches the whole seed, so the warning is far starker
 * than the account-key QR's: that one only ever exposes future addresses,
 * this one is every key the mnemonic can ever derive. There is no camera to
 * scan the result back with, so tools/origo_verify.py inspect prints the same
 * payload for an independent check instead. */
/* Index 0 is the full code, seen at its natural size first; index 1 is the
 * region map (that same code, with the boundaries and labels each zoomed
 * tile beyond it will use); 2..regions+1 are Krux-style "Zoomed Region"
 * tiles, stepped sideways the same way show_qr steps between values, so the
 * carousel convention stays one shape everywhere a QR is shown.
 *
 * One past the last tile is the sun, the only stop that is a control rather
 * than a view. These are separate views of separate content, not frames of one
 * payload the way the account key's are, so up and down cannot be freed here
 * the way they were there - the shade earns its place in the ring instead.
 * The view under it stays whichever one was being read, so the shade changes
 * on the code the reader already has a camera pointed at. */
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
            const bool ok = view == 0
                ? seedtool_display_qr_bytes("Compact SeedQR", entropy, len, chrome)
                : view == 1 ? seedtool_display_qr_bytes_map("Compact SeedQR", entropy, len, chrome)
                            : seedtool_display_qr_bytes_region("Compact SeedQR", entropy, len, view - 2, chrome);
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

/* The three things that decide what every other wallet screen derives: which
 * account, which script type, and which passphrase. They used to be scattered
 * - account a row on the Wallet menu, type a fork into one of two sub-menus
 * each owning its own copy of Account key/Descriptor/Addresses, and passphrase
 * a gate asked once before the Wallet menu existed and unchangeable after -
 * so setting all three meant crossing screens that had nothing else in common.
 * Gathered here they read as what they are, and as what names the screen: the
 * inputs to the derivation, editable in any order, none of them a place you
 * pass *through*. The account and the type choose the path; the passphrase
 * goes into the PBKDF2 above it, so it decides the seed the path is walked
 * from. Different levels, one question - where the keys come from.
 *
 * `fp`/`fphex` come in by pointer because the passphrase is one of the two
 * inputs to the master fingerprint: change it and the fingerprint titling the
 * Wallet menu is stale, so it is re-derived here rather than left to disagree
 * with what the device is now deriving from.
 *
 * Follows show_settings_menu's shape exactly - live-formatted labels rebuilt
 * each pass, a row either toggling in place or opening a dedicated editor and
 * returning. */
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
        /* Ordered by how deep each one cuts. The passphrase decides the seed
         * itself, so changing it changes every key the device can produce and
         * the fingerprint with them; the type picks a path from that seed; the
         * account picks a branch of that path. Widest consequence first, and
         * the fingerprint in the title above only ever moves for the first. */
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

    /* Starts with no passphrase and derives immediately, rather than asking
     * for one before anything can be seen. The passphrase is a parameter of
     * the derivation like the account and the type, and it is set in the same
     * place they are; a gate here made it the one parameter that had to be
     * decided before the wallet existed and could not be revisited after.
     * Which of the two is in force is stated rather than merely defaulted to:
     * in full on Derivation's own row, and by the fingerprint titling the menu
     * below, which is a function of it and moves whenever it changes. */
    if (seedtool_master_fingerprint(mnemonic, passphrase, fp) != SEEDTOOL_OK) {
        notice("Error", "Derivation failed", NULL);
        goto done;
    }
    hexstr(fp, sizeof(fp), fphex);

    /* m/type'/0'/account' and the passphrase above it: all three live for the
     * whole wallet-viewing session rather than one visit to a screen, so
     * checking account 2 under both types means setting it once - not
     * resetting on the way from one to the other. They are edited together in
     * Derivation, which is what they have in common: each is an input to what
     * the other screens derive. Backup reads none of them, being about the
     * mnemonic itself rather than anything derived from it, and neither does
     * the fingerprint in the title, which is always the root's. */
    uint32_t account = 0;
    seedtool_address_type_t type = SEEDTOOL_BIP84;
    /* The fingerprint names this menu rather than sitting on a row of it: it
     * is an identity, not an action, and a row that only displayed it was a
     * screen entered to read one value and left again. In the title it is read
     * without being asked for, which is what makes it the check it is for -
     * it is a function of the passphrase in force and moves the moment that
     * does, so a reader who knows their wallet's fingerprint can see at a
     * glance whether the device is deriving that wallet. Whether a passphrase
     * is set at all is said in full on Derivation's own row. */
    char wallet_title[sizeof("Wallet @") + 8];
    _Static_assert(sizeof(wallet_title) >= sizeof("Wallet @") + 8,
        "the wallet title must hold its label and all eight fingerprint hex digits");
    (void)snprintf(wallet_title, sizeof(wallet_title), "Wallet @%s", fphex);
    size_t cursor = 0;
    for (;;) {
        const char* menu[] = { "Backup", "Extended public key", "Derivation", "Addresses", "Erase and restart" };
        /* The chrome without an arrow: there is no level above a loaded
         * wallet, and its only exit erases the session. That stays a row the
         * reader travels to rather than a control one press from anywhere. */
        const int selected
            = choose_nav(wallet_title, menu, sizeof(menu) / sizeof(menu[0]), NULL, false, false, &cursor);
        /* Both ways out of a wallet session reboot: the row, and the timeout
         * that means the reader walked away from a device with a seed on it.
         * Unwinding instead would leave every buffer between here and the main
         * menu holding what it last held, wiped only where someone remembered
         * to wipe it. A restart re-runs C startup, which zeroes .bss - the
         * address cache and the review rows live there - and leaves nothing on
         * a stack that is about to be reused from the top. The explicit wipes
         * below still run first: the reboot is the belt, not a reason to drop
         * the braces.
         *
         * The cost is the display settings, which are RAM-only by design and
         * so go back to their defaults. That is the trade this makes
         * deliberately: brightness is a preference, and the seed is not. */
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

/* A backup is only as good as its transcription: quizzes one word from every
 * group of three consecutive words in the mnemonic just shown, against what
 * was actually generated - catching a copying mistake here rather than the
 * day it matters, the same re-type-and-compare idea enter_passphrase_once
 * already uses to confirm a passphrase. The grouping matches Blockstream
 * Jade's own confirmation step (display_confirm_mnemonic in its
 * process/mnemonic.c: one word quizzed per run of three, so every word is at
 * least shown) rather than a flat count regardless of length - 4 checks for
 * 12 words, 8 for 24, not 3 either way. Unlike Jade, the word picked within
 * each group is fixed (always the middle one) rather than random: "no
 * screen in the entry path may depend on the device RNG" (enter_word's own
 * doc comment) still applies here, and a fixed middle word already reaches
 * every group at least as well as a random pick within it would. A wrong
 * word restarts the whole quiz from the first group, the same as Jade's own
 * display_confirm_mnemonic - a soft warn-and-proceed version of this was
 * tried first, but let a reader who mistyped a word straight through to the
 * wallet having been told and then ignored, which defeats the point of
 * asking. Backspace past the start of a quiz word steps back to the
 * previous one, same as enter_mnemonic_words - it is not a wrong answer,
 * and treating it as one is what made this feel like it was scoring a
 * random keypress instead of asking to go back. Backing out of the first
 * quiz word of a pass, or declining the try-again prompt after a miss,
 * leaves confirmation altogether - but unlike most other optional steps
 * here, that cannot silently fall through to whatever comes next (the
 * wallet's passphrase prompt): show_generated shows the words again
 * instead, since back is meant to step back one stage, not skip forward
 * past confirmation into an unrelated screen. Returns 1 once a full pass
 * matches, 0 on giving up (either way above), -1 on timeout, the same
 * three-way contract every other entry screen here uses. */
static int confirm_backup(const char* mnemonic, const size_t count)
{
    /* The canonical parser (also show_numbered_list's), not a hand-rolled
     * space-split: it validates every word against the wordlist on the way
     * in, so a malformed mnemonic is caught here rather than confirm_backup
     * quizzing against a substring nobody checked. */
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

/* The words, the reminder, the quiz, and the wallet the quiz unlocks. Returns
 * false only when the reader steps back off the first of those, so a caller
 * still holding what produced the mnemonic can show it again rather than
 * treating one press as the end of the visit.
 *
 * Stages, not a chain of && and a while: back has to step back one screen,
 * and a short-circuit chain has exactly one way out of it - the whole
 * function. Each back here is the stage before it, and only the first leaves. */
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
            /* The word list is left by taking its confirm bar, so without
             * this screen the quiz's keyboard simply appeared, with no
             * warning that the words were about to leave. The quiz's own
             * "word N of M" title says which word it wants but never why it
             * is asking, so the count is named here instead.
             *
             * `intro` is sized for the format's worst case rather than its
             * real one: the counts are 4/12 or 8/24, but %u lets the compiler
             * assume ten digits apiece, and -Wformat-truncation is an error
             * in the firmware build even though the host build lets it pass. */
            char intro[48];
            (void)snprintf(
                intro, sizeof(intro), "Retype %u of the %u words", (unsigned)(words / 3), (unsigned)words);
            const int taken
                = nav_screen("Confirm backup", intro, "Have your backup ready", "Start quiz", true, false, NULL);
            /* A timeout ends it here rather than stepping back: repainting
             * the whole mnemonic on the way out of an expired session is the
             * one thing this must not do. */
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

/* Bits collected so far and whether the run looks patterned, for any of the
 * four sources - the one place that decides which source-appropriate method
 * to use, so the live bar below and collect_entropy's end-of-run gate can
 * never disagree. Cards drawn without replacement (SEEDTOOL_CARDS) have
 * exact bits (seedtool_card_entropy_bits), not estimated, and carry no bias
 * correction. Everything else here - D6/D20/coin, and a with-replacement
 * card draw (SEEDTOOL_CARDS_REPLACE), a genuine 52-sided die - shares the
 * same plug-in Shannon estimator; coin's 0/1 and card's 0..51 both become a
 * 1-indexed face for this call only - a local copy, never the canonical
 * values[] the transcript is built from - since seedtool_dice_entropy_bits
 * expects every source it covers to be 1-indexed like D6/D20 already are. */
static void entropy_quality(
    const seedtool_source_t source, const uint8_t* values, const size_t count, int* bits_out, bool* pattern_out)
{
    if (source == SEEDTOOL_CARDS) {
        (void)seedtool_card_entropy_bits(count, bits_out);
        (void)seedtool_card_pattern_detected(values, count, pattern_out);
        return;
    }
    /* Zero-initialized, not left bare: the loop below only fills faces[0..
     * count-1], and GCC's -Wmaybe-uninitialized can't always prove that
     * partial fill is enough once faces_ptr crosses the opaque pointer
     * boundary into seedtool_dice_entropy_bits - a false positive under
     * -O2's constant propagation, but a real bare array here would leave
     * bytes past count genuinely uninitialized if anything ever read past
     * it. */
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
    /* faces holds a shifted copy of the real coin/card draws whenever it was
     * filled above - the same secret values as `values`, just re-indexed for
     * seedtool_dice_entropy_bits. Called on every keystroke while entropy is
     * being collected, so this would otherwise leave many stale copies of
     * live entropy scattered across old stack frames. */
    seedtool_zero(faces, sizeof(faces));
}

/* Live quality readout for a run in progress: draws collected and bits so
 * far, each against what the seed needs. Adapted from Krux's dice-roll
 * entropy screen (github.com/selfcustody/krux,
 * src/krux/pages/new_mnemonic/dice_rolls.py) and extended here to every
 * source - a display of what has already been entered, not an input to what
 * gets hashed. */
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
    /* "Coins", not the menu's own "Coin flips": these label the entry screen,
     * whose title also carries the position counter and the bit count, and
     * coin is the source with the longest counter (256/256) as well as the
     * longest name. Spelled out at full length it left the title 12px from
     * the glass, inside the inset the quality bar below it keeps. The menu
     * that got the reader here still says "Coin flips"; by this screen the
     * source is not in doubt. */
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
        /* One deck can't reach 256 bits without replacement (see
         * SEEDTOOL_CARDS_REPLACE's doc comment), so this is the one source
         * whose upfront screen has a physical process to explain rather
         * than just a red-bar hint. Kept to one line at this font/width -
         * the longer phrasing wrapped its second line down into the quality
         * bar drawn right below. */
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
            /* Rebuilt from the values each time rather than appended to, so what is
             * on screen is the transcript itself and cannot drift from it — going
             * back a step has to unwrite the last entry too. */
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
                /* A with-replacement draw excludes nothing, so it passes no
                 * availability mask - enter_card treats NULL the same way
                 * enter_value already treats a NULL allowed[]. */
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
        /* The whole run is fixed-length, unlike Krux's open-ended rolling: there
         * is no "keep going a bit more" here, only "go fix a roll" — declining
         * below steps back into the entry loop at the last one, the same way a
         * manual [back] already does. */
        int bits = 0;
        bool pattern = false;
        entropy_quality((seedtool_source_t)source, values, required, &bits, &pattern);
        const bool poor = (size_t)bits + DICE_ENTROPY_TOLERANCE < min_bits;
        bool proceed = true;
        /* Shown on both the poor-entropy and the looks-good screen below, so
         * either way the reader sees the number the accept/reject decision
         * was actually made on, not just the verdict. */
        char bits_line[24];
        (void)snprintf(bits_line, sizeof(bits_line), "%d of %u bits", bits, (unsigned)min_bits);
        /* These two carry their verdict in the body rather than the title bar,
         * which is the one place the chrome's usual rule is set aside. A title
         * is read as the name of the screen you are on; these are not names but
         * findings about the run just made, and a finding that the seed may be
         * weak should land where the eye already is - the middle of the glass -
         * not in the strip the reader has learned to skim. The bar keeps the
         * arrow and the tick, so the way out and the way on are unchanged. */
        if (poor) {
            proceed = nav_acknowledge(NULL, "Poor entropy!", bits_line, "Proceed anyway", true);
        }
        if (proceed && pattern) {
            proceed = nav_acknowledge(NULL, "Pattern detected!", NULL, "Proceed anyway", true);
        }
        if (proceed && !poor && !pattern) {
            /* The positive case: the bar's outline goes green, the one point in
             * the run where seedtool_progress_t.complete is ever true - but the
             * verdict is also spelled out in the question itself, not left to
             * the border colour alone, since not every reader (or every
             * lighting condition) tells a green outline from a dim one at a
             * glance. */
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
            /* Stepped back off the first screen after generating. The rolls
             * are all still here, so this is the verdict again - not the run
             * thrown away and started from the first roll. */
            continue;
        }
        --i;
        /* The same restore the in-run back-step does above. Without it,
         * declining this screen on a without-replacement card run stepped
         * back to the last card with that card still struck off the deck, so
         * it could not be re-picked - and every further decline struck off
         * one more. */
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

/* The BIP39 English wordlist is exactly 2048 long, so eleven coin flips name
 * one word and every word is equally reachable - no rejection sampling, no
 * modulo that would favour the front of the list. Most significant bit first,
 * the same order seedtool_complete_checksum packs those indices back into
 * entropy in (seedtool_core.c), so what the reader sees on screen is the
 * arithmetic the seed is actually built from rather than a re-encoding of it. */
#define COIN_WORD_BITS 11

/* Eleven flipped words are 121 bits and twenty-three are 253; the entropy needs
 * 128 and 256. The remainder is flipped loose at the end, and BIP39's checksum
 * supplies the last 4 or 8 bits of the final word. */
static size_t coin_flipped_words(const size_t words) { return words == 12 ? 11 : 23; }
static size_t coin_tail_bits(const size_t words) { return words == 12 ? 7 : 3; }

/* One word's worth of flips. Returns 1 with `*index_out` set to the zero-based
 * wordlist index, 0 when the reader backed out of the word's first flip, and -1
 * on timeout - the same contract as enter_value() and enter_word(). Backing out
 * of any later flip undoes that flip rather than the whole word, so a slip on
 * bit 9 of 11 costs one press, not eleven. */
static int enter_flipped_word(const unsigned position, const unsigned total, uint16_t* index_out)
{
    uint8_t bits[COIN_WORD_BITS] = { 0 };
    /* Sized for the format's worst case, not its real one: the counts here are
     * 11 or 23, but %u lets the compiler assume ten digits apiece, and
     * -Wformat-truncation is an error in the firmware build. */
    char title[48];
    (void)snprintf(title, sizeof(title), "Word %u of %u", position, total);
    int outcome = 1;
    size_t i = 0;
    while (i < COIN_WORD_BITS) {
        /* Rebuilt per flip rather than kept alongside `bits`: the flips are
         * secret, and a single buffer that only ever holds what is currently
         * on screen is one fewer copy to wipe. */
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

/* The flips, the wordlist number they spell, and the word itself, on one screen
 * before moving on. This is the whole point of entering a word at a time: the
 * reader can check this line against a printed wordlist with no device
 * involved, which is exactly what the hashed coin path cannot offer.
 *
 * The number is one-based and zero-padded to four digits, matching
 * show_numbered_list and enter_word_number - a printed list pads to four, and
 * `abandon` is 1 there, not 0, even though the encoding above is zero-based.
 *
 * Going back from here costs eleven flips, so unlike every other back in the
 * run it is asked about rather than acted on: Up/Down raises the question and
 * BOTH answers it, which also means a stray press on a screen the reader is
 * only reading cannot silently undo the word they just checked. The cheap
 * backs - one flip inside a word - stay unguarded, since a guard there would
 * cost more presses than the mistake does.
 *
 * Returns 1 to accept, 0 to flip the word again, -1 on timeout. */
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
        /* Number and word share a line so the chrome's two body lines carry
         * all three facts the reader is checking: the flips, the position in
         * the list, and what it spells. */
        const int key = nav_screen(title, bitline, named, "Use this word", true, false, NULL);
        if (key == NAV_TIMEOUT) {
            outcome = -1;
            break;
        }
        if (key == NAV_CONFIRM) {
            outcome = 1;
            break;
        }
        /* The word is named again on the question rather than left to the
         * screen behind it: this is the last place it is shown before those
         * flips go, and a reader who pressed by accident should be able to see
         * what they are about to discard without dismissing the question to
         * find out. */
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

/* Coins, but packed straight into the entropy instead of hashed: eleven flips
 * per word for the first 11 (or 23) words, then the 7 (or 3) bits that finish
 * the entropy, and BIP39's checksum completes the last word. Same arithmetic as
 * complete_checksum()'s, and the same seedtool_complete_checksum() call - the
 * difference is only that the words are flipped here rather than typed in from
 * a wordlist the reader converted by hand.
 *
 * There is no quality bar and no Shannon gate on this path, unlike the hashed
 * one. Nothing here is being estimated: eleven flips are eleven bits by
 * construction, and a gate could only second-guess the reader's coin.
 *
 * Same contract as create_seed(): true once a mnemonic reached the wallet
 * viewer or the session timed out, false for backing out before anything was
 * flipped. */
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

    /* Set when the run steps backwards onto a word that was already flipped:
     * that word reopens at its confirmation, showing what it still is, rather
     * than throwing the flips away and demanding eleven more. Accepting there
     * walks forward again with the word unchanged; declining reflips it. */
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
                    /* Backed out of this word's first flip, where the word has
                     * no flip of its own left to undo. That press means "the
                     * word before this one was wrong", so it reopens that word.
                     * The run ends here only from the very first flip of the
                     * very first word - the one point where nothing has been
                     * entered yet.
                     *
                     * Ending it anywhere else was a bug: eight words in, one
                     * press dropped the lot and landed back on the length
                     * picker, which is exactly the mistake a word-at-a-time
                     * screen exists to make cheap. */
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

        /* The loose bits that finish the entropy. Backing out of the first of
         * them returns to the last word, so back walks the whole run in one
         * direction instead of dead-ending here. */
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
            /* Reopened at its confirmation, like every other backwards step:
             * the last word is still perfectly good, and the reader who
             * pressed back here may well have been after the word before it. */
            w = word_count - 1;
            reopen = true;
            continue;
        }

        prefix[0] = '\0';
        size_t used = 0;
        for (size_t i = 0; i < word_count; ++i) {
            const char* const word = seedtool_word(indices[i]);
            const size_t len = strlen(word);
            /* Bounded by SEEDTOOL_MAX_MNEMONIC_LEN, which holds 24 words -
             * this builds at most 23 - but checked rather than assumed, since
             * a silent truncation here would be a wrong seed, not a wrong
             * screen. */
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

/* Which of the two coin derivations to run. Only coins get this screen: they
 * are the one source whose transcript a reader can convert to words by hand,
 * so they are the only one where packing the bits directly buys anything over
 * hashing them. Returns 0 for the flipped-word path, 1 for the hashed one, and
 * -1 for back. */
static int choose_coin_method(void)
{
    const char* methods[] = { "Flip each word", "Flip and hash" };
    const int selected = choose_menu("Coin method", methods, 2);
    return selected < 0 || selected == 2 ? -1 : selected;
}

/* Returns whether a seed was actually generated, as opposed to the reader
 * backing out of the source or length picker before ever starting.
 * show_new_seed_menu uses this to tell "done, go all the way home" apart from
 * plain "back one level".
 *
 * Since leaving the wallet viewer reboots, the true case now only reaches its
 * caller when the viewer returned without ever opening - a derivation failure
 * on the way in. The distinction still has to exist for that, and for the
 * false case, which is the ordinary back-out. */
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
                    /* Backed out of the very first flip, before anything was
                     * entered: this menu again, one stage back, rather than
                     * skipping past it to the length picker. */
                }
                if (!run_hashed) {
                    continue;
                }
            }
            /* 24-word Cards can't reach 256 bits without replacement (one
             * deck tops out around 225 bits) - SEEDTOOL_CARDS_REPLACE draws
             * the same deck, but with the card returned and the deck
             * reshuffled after every draw, a genuine 52-sided die instead. */
            const int collect_source = length && source == SEEDTOOL_CARDS ? SEEDTOOL_CARDS_REPLACE : source;
            if (collect_entropy(collect_source, words) != 0) {
                return true;
            }
        }
    }
}

/* Same contract as create_seed(): true once a completed mnemonic actually
 * reached the wallet viewer (or a timeout), false for backing out of the
 * length picker or the very first word before anything was entered. */
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
        /* The words are held here rather than inside the entry helper, so that
         * stepping back out of the flips can hand them back instead of
         * starting over. `by_number` rides along for the same reason: a
         * re-opened word should be fixed the way it was typed. */
        char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
        bool by_number = false;
        int outcome = enter_mnemonic_words(count, words, &by_number);
        if (outcome == 1 && !join_words(words, count, prefix, sizeof(prefix))) {
            outcome = -1;
        }
        /* The flips and the result they produce sit in one loop, so that
         * backing off the completed mnemonic is the last flip again rather
         * than the whole thing discarded: `i` outlives the flip loop for
         * exactly that step back. */
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
                    /* Backing out of the first flip returns to the words - to the
                     * list of them, with every one still there, rather than to an
                     * empty entry screen. */
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
                /* Said out loud rather than fallen through silently, which is
                 * what this used to do: the reader typed eleven words and flipped
                 * seven coins and was handed back the menu with no reason. */
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

/* Two ways to end up with a seed to work with: build one from entropy, or
 * finish one that is already mostly known (its words come from elsewhere -
 * typically a physical Stackbit backup - and only the checksum word is
 * missing). Grouped under one menu since both lead to the same wallet
 * viewer, rather than sitting as peers of Restore/Settings on the main
 * menu, which is specifically about *entering* an already-complete seed. */
static void show_new_seed_menu(void)
{
    size_t cursor = 0;
    for (;;) {
        const char* items[] = { "From entropy", "Complete checksum" };
        const int selected = choose_menu_at("New Seed", items, 2, &cursor);
        if (selected < 0 || selected == 2) {
            return;
        }
        /* A seed that was generated closes this menu too, straight back to the
         * Origo/Home menu, rather than reopening "New Seed" - that reopening
         * was the actual bug: ending a session landed back inside New Seed
         * instead of at Home. Leaving the wallet viewer now reboots, which
         * settles that case before it gets here; what is left for this to
         * handle is the viewer failing to open at all. Backing out of the
         * source/length picker before anything was generated keeps the old
         * "one level up" behaviour. */
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

/* Applied live, one step per press, same as the coin flip / dice value
 * convention: KEY_PREV raises, KEY_NEXT lowers. There is nothing to confirm -
 * the level already shown on screen is the level in effect - so KEY_SELECT
 * and a timeout both just leave. */
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
    /* Throwing the splash's presses away is not enough on its own: a button
     * still held as the menu takes over would arrive there as a press the menu
     * never saw begin. */
    seedtool_platform_flush_keys();
    last_action = seedtool_platform_milliseconds();

    size_t cursor = 0;
    for (;;) {
        const char* menu[] = { "New Seed", "Restore Seed", "Settings" };
        /* No arrow: this is the top, and the only way past it is the board's
         * own reset. It never carried the chord hint either - it is the first
         * screen after the splash, and teaching two things at once was already
         * ruled out - so wearing the chrome costs it nothing it had. */
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
