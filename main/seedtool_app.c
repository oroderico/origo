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
#define ACK_FOOTER "BOTH continue   Up/Down back"

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

static void screen_text3(
    const char* title, const char* line1, const char* line2, const char* line3, const char* footer)
{
    seedtool_display_screen3(title, line1, line2, line3, footer);
}

static void screen_text4(const char* title, const char* line1, const char* line2, const char* line3,
    const char* line4, const char* footer)
{
    seedtool_display_screen4(title, line1, line2, line3, line4, footer);
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

/* Same contract as acknowledge(), but for the dice-entry screen with its
 * quality bar drawn in, rather than plain text — used before a D6/D20 run
 * starts, showing the bar empty, and once more after it ends, showing it full. */
static bool dice_confirm(const char* title, const char* one, const char* two, const seedtool_progress_t* progress)
{
    for (;;) {
        seedtool_display_dice_screen(title, one, two, ACK_FOOTER, progress);
        const seedtool_key_t key = wait_key();
        if (key != KEY_REDRAW) {
            return key == KEY_SELECT;
        }
    }
}

/* Every choice in the firmware is made here, on a list that shows five options
 * at once. Which rows are on screen follows from the count and the selection
 * alone, so a choice screen is reproducible from what the user has done.
 * `hint` is false only for the Origo menu: the very first screen after the
 * splash is not the place to also be teaching the chord, and every screen
 * reachable from it already carries its own hint until the chord is learned. */
static int choose_at(const char* title, const char* const* items, const size_t count, const bool hint,
    const size_t initial)
{
    size_t selected = initial < count ? initial : 0, top = 0;
    for (;;) {
        /* No page counter here: choosing among options isn't paging through
         * content, and the scrollbar (seedtool_render_list, gated the same way
         * on count > SEEDTOOL_LIST_ROWS) already shows position when the list
         * doesn't fit on screen. */
        const char* const footer = (hint && !chord_learned) ? NAV_FOOTER : "";
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

static int choose(const char* title, const char* const* items, const size_t count, const bool hint)
{
    return choose_at(title, items, count, hint, 0);
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
        if (progress) {
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
 * No footer hint: NAV_FOOTER's "L/R move BOTH select" would be wrong here —
 * L/R commit a choice rather than moving one, and BOTH undoes rather than
 * selects — and every path that reaches this screen already went through at
 * least one chord-gated menu first, so chord_learned is always true by then
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
 * the tighter line pitch seedtool_render_screen3 uses: a third fewer pages to
 * review the same value. Returns true when the reader advanced past the last
 * page or accepted, false when they backed out. */
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
    const size_t pages = (lines + 2) / 3;
    size_t page = 0;
    bool advanced;
    /* Hoisted out of the loop so the single exit below can wipe them: what
     * these three hold is whatever the caller is paging, and the callers page
     * the canonical transcript, its SHA256 and the mnemonic itself. `start`
     * and `length` are offsets into the caller's own string and carry nothing
     * on their own. */
    char line1[MAX_LINE_CHARS + 1], line2[MAX_LINE_CHARS + 1], line3[MAX_LINE_CHARS + 1];
    for (;;) {
        char footer[48];
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
        (void)snprintf(footer, sizeof(footer), "%u/%u%s", (unsigned)(page + 1), (unsigned)pages, nav_hint());
        screen_text3(title, line1, line2, line3, footer);
        switch (wait_key()) {
        case KEY_SELECT:
            advanced = true;
            goto done;
        case KEY_NEXT:
            if (page + 1 >= pages) {
                advanced = true;
                goto done;
            }
            ++page;
            break;
        case KEY_PREV:
            if (!page) {
                advanced = false;
                goto done;
            }
            --page;
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

/* Frames auto-advance with no press needed; BOTH/timeout leave the screen,
 * same as a static QR would. */
#define BBQR_FRAME_INTERVAL_MS 700

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
/* Shared by both places show_bbqr steps a frame by hand (already manual, and
 * the moment auto-advance switches to manual), so the wraparound at each end
 * of the frame range is one expression, not two copies to keep in sync. */
static size_t bbqr_step_part(const size_t part, const size_t parts, const bool forward)
{
    return forward ? (part + 1) % parts : (part + parts - 1) % parts;
}

static seedtool_key_t show_bbqr(const char* title, const char* value)
{
    const size_t len = strlen(value);
    const size_t frame_chars = seedtool_render_qr_alphanumeric_capacity(BBQR_FRAME_MAX_VERSION);
    const size_t parts = seedtool_bbqr_part_count(len, frame_chars);
    if (!parts) {
        (void)acknowledge("Too long for a QR", title, "Read it as text instead");
        return KEY_SELECT;
    }
    size_t part = 0;
    /* Once the reader steps a frame by hand, auto-advance stops for the rest
     * of this screen: the 700ms cadence is a guess at what a given camera
     * needs, and a reader who has already found that guess wrong is better
     * served by a still frame they step themselves than by the same guess
     * still running underneath them. A single-part value has nothing to
     * step to, so it starts (and stays) in that same still mode. */
    bool manual = parts <= 1;
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
            && seedtool_display_qr(frame_title, frame);
        seedtool_zero(frame, sizeof(frame));
        if (!ok) {
            (void)acknowledge("Too long for a QR", title, "Read it as text instead");
            return KEY_SELECT;
        }
        if (manual) {
            const seedtool_key_t key = wait_key();
            if (key == KEY_REDRAW) {
                continue;
            }
            if (key == KEY_PREV || key == KEY_NEXT) {
                part = bbqr_step_part(part, parts, key == KEY_NEXT);
                continue;
            }
            return key;
        }
        bool ticked = false;
        const seedtool_key_t key = wait_key_or_tick(BBQR_FRAME_INTERVAL_MS, &ticked);
        if (ticked) {
            part = (part + 1) % parts;
            continue;
        }
        if (key == KEY_REDRAW) {
            continue;
        }
        if (key == KEY_PREV || key == KEY_NEXT) {
            manual = true;
            part = bbqr_step_part(part, parts, key == KEY_NEXT);
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
            items[matches] = "[delete]";
            (void)snprintf(listing, sizeof(listing), "%s  %s", title, stem_len ? stem : "-");
            const int chosen = choose(listing, items, matches + 1, true);
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
            (void)seedtool_next_letters(stem, stem_len, letters);
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
 * is otherwise a different seed with no sign that anything went wrong. */
static int enter_word_number(const size_t position, const size_t total, char* output, const size_t output_len)
{
    char digits[SEEDTOOL_MAX_WORD_DIGITS + 1] = { 0 };
    size_t digits_len = 0;
    size_t selected = seedtool_layout_center(WORD_NUMBER_LAYOUT);
    const size_t accept_index = layout_key_index(WORD_NUMBER_LAYOUT, SEEDTOOL_KEY_ACCEPT);
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
        /* Once the typed digits are themselves a complete valid number and
         * cannot be extended any further, every digit key goes dark at once
         * and the cursor has to land somewhere. nearest_enabled's ring
         * distance to Accept vs. Backspace from here depends on exactly
         * which key the reader was last on - circumstantial, and it lands on
         * Backspace as often as not. Accept is the deliberate target
         * whenever it's actually reachable; only fall back to the generic
         * search when it isn't (still mid-prefix, no complete number yet). */
        selected = !enabled[selected] && number ? accept_index : nearest_enabled(enabled, WORD_NUMBER_KEYS, selected);
        bool picked = false;
        while (!picked) {
            seedtool_display_keyboard(
                title, digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected, position, total);
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

/* Returns 1 when a whole mnemonic was entered, 0 when the user backed out of the
 * first word or the method menu, -1 on timeout or overflow. Fills `words` (a
 * caller-owned array so restore_mnemonic below can keep reviewing them after
 * entry finishes) and, if `by_number` is not NULL, reports which entry method
 * was used - re-entering a single word during review needs the same one. */
static int enter_mnemonic_words(
    const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number)
{
    const char* methods[] = { "Type the letters", "Enter word numbers", "Back" };
    const int method = choose("Word entry", methods, 3, true);
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
    return outcome;
}

/* words[0..count) joined with single spaces, the same layout seedtool_generate's
 * transcript-to-mnemonic conversion and every published BIP39 vector use.
 * Shared by enter_mnemonic (once, after entry) and review_and_confirm (again
 * after every edit), so a mid-review fix is checked against the exact string
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

static int enter_mnemonic(const size_t count, char* mnemonic, const size_t mnemonic_len)
{
    char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
    int outcome = enter_mnemonic_words(count, words, NULL);
    if (outcome == 1 && !join_words(words, count, mnemonic, mnemonic_len)) {
        outcome = -1;
    }
    seedtool_zero(words, sizeof(words));
    return outcome;
}

static char review_labels[24][32];
static const char* review_items[24 + 2]; /* + "Continue"/status, + "Back" */

/* Lets the reader jump straight to any already-entered word and fix it,
 * rather than losing the other 11 or 23 correct ones over a single mistake -
 * restore_seed used to just discard the whole entry and show "INVALID
 * CHECKSUM" with no way back in. A scrolling list (choose_at, cursor
 * persisted across edits), the same widget and pattern the address list
 * already uses to pick one item out of several to inspect or act on -
 * coherence with the rest of the app mattered more here than the carousel's
 * one-item-at-a-time feel, since this is fundamentally "pick which of these
 * to fix," not "read through them in order." "Continue" (or the checksum's
 * current verdict) and "Back" ride the same list as two more rows, past the
 * last word. Shown after every full entry, not only a failed one, so a word
 * can be double-checked before continuing at all. `words` is edited in
 * place; `mnemonic` is rejoined from it after every change so
 * seedtool_validate_mnemonic always grades the current state. Returns 1 once
 * Continue is chosen with a valid checksum, 0 on explicit Back, -1 on
 * timeout or overflow - the same three-way contract every other entry
 * screen here uses. */
static int review_and_confirm(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, const bool by_number,
    char* mnemonic, const size_t mnemonic_len)
{
    size_t cursor = 0;
    int outcome;
    /* Cleared at both ends, exactly as derive_addresses does with
     * address_labels: these rows are the mnemonic in plain text, one word
     * each, and they live in .bss rather than on a stack frame that the next
     * screen would overwrite anyway. Leaving them behind kept a restored seed
     * readable in RAM for the rest of the session - past "Done / erase" and
     * past the session-timeout screen, which erases nothing here. Clearing on
     * entry as well as exit keeps a 12-word restore from leaving rows 12..23
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
        /* Continue only appears once it would actually do something: an
         * invalid checksum can't be acted on, so an inert "Checksum invalid"
         * row that just redisplays the same list on select was a dead click
         * - the title already says "Review - fix a word", no extra row
         * needed to repeat that. */
        size_t entries = count;
        if (valid) {
            review_items[entries++] = "Continue";
        }
        review_items[entries++] = "Back";
        const int selected
            = choose_at(valid ? "Review words" : "Review - fix a word", review_items, entries, true, cursor);
        if (selected < 0) {
            outcome = -1;
            goto done;
        }
        if ((size_t)selected == entries - 1) {
            outcome = 0;
            goto done;
        }
        if (valid && (size_t)selected == count) {
            outcome = 1;
            goto done;
        }
        cursor = (size_t)selected;
        char word[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
        const int result = by_number ? enter_word_number(selected + 1, count, word, sizeof(word))
                                      : enter_word(selected + 1, count, word, sizeof(word));
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

/* enter_mnemonic_words, then review_and_confirm on the result: the two-step
 * restore_seed actually wants. Not folded into enter_mnemonic itself since
 * complete_checksum also calls that for a still-partial (11 or 23 word)
 * mnemonic that could never pass seedtool_validate_mnemonic yet - the review
 * gate belongs only where the mnemonic is meant to be whole and correct.
 * join_words and seedtool_validate_mnemonic are the same pair review_and_
 * confirm itself calls every time it redraws; reused once more here, before
 * the first draw, so an invalid checksum is announced up front rather than
 * only implied by "Review words" silently reading "Review - fix a word"
 * instead - the inert "Checksum invalid" row this screen used to carry (see
 * "Drop the inert 'Checksum invalid' row from word review") was removed for
 * being a dead click, not because the reader shouldn't be told; a one-shot
 * acknowledge, the same widget restore_seed's own "Checksum valid" already
 * uses for the opposite verdict, says it without adding a row that does
 * nothing when picked. */
static int restore_mnemonic(const size_t count, char* mnemonic, const size_t mnemonic_len)
{
    char words[24][SEEDTOOL_MAX_WORD_LEN + 1] = { { 0 } };
    bool by_number = false;
    int outcome = enter_mnemonic_words(count, words, &by_number);
    if (outcome == 1) {
        if (join_words(words, count, mnemonic, mnemonic_len)
            && seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK) {
            (void)acknowledge("Invalid checksum", "Check your words", "Fix one to continue");
        }
        outcome = review_and_confirm(words, count, by_number, mnemonic, mnemonic_len);
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

/* The optional passphrase, set and changed from Customize rather than asked
 * for on the way in. A session starts without one and says so on Customize's
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
    const char* options[] = { "No passphrase", "Enter passphrase", "Back" };
    const int selected = choose("Passphrase", options, 3, true);
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
        && acknowledge("Confirm passphrase", "Enter it a second time", "Exact match required")
        && enter_passphrase_once(confirmation, sizeof(confirmation)) && strcmp(attempt, confirmation) == 0;
    if (ok) {
        /* The old value goes before the new one lands, not after: this buffer
         * holds a live session secret and must never briefly hold a mix. */
        seedtool_zero(passphrase, SEEDTOOL_MAX_PASSPHRASE_LEN + 1);
        memcpy(passphrase, attempt, sizeof(attempt));
    } else {
        (void)acknowledge("Passphrase mismatch", "Passphrase unchanged", "Try again");
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
    if (!acknowledge("QR export", "Account key included", "A photo reveals every address")) {
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
        (void)acknowledge("Error", "Could not derive", NULL);
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
    if (!acknowledge("Descriptor export", "Account key included", "A photo reveals every address")) {
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
            (void)acknowledge("Error", "Could not derive", NULL);
        }
    } else {
        seedtool_zero(xpub, sizeof(xpub));
        (void)acknowledge("Error", "Could not derive", NULL);
    }
    seedtool_zero(body, sizeof(body));
    seedtool_zero(value, sizeof(value));
}

/* A single address's own QR, opened from the address list: no account key in
 * this view to warn about or to carousel over to, since a photo of one
 * address on its own reveals nothing the address itself did not already. */
static void show_address_qr(const char* title, const char* address)
{
    for (;;) {
        if (!seedtool_display_qr(title, address)) {
            (void)acknowledge("Too long for a QR", title, "Read it as text instead");
            return;
        }
        if (wait_key() != KEY_REDRAW) {
            return;
        }
    }
}

/* Addresses and their list labels are derived once per visit to the Addresses
 * screen and cached here for the whole visit, not re-derived every time the
 * reader backs out of one address's QR back to the list - only leaving the
 * screen for good retires the cache (see its callers in show_type_menu).
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
    const uint32_t account, const seedtool_chain_t chain)
{
    /* Cleared on entry as well as on the way out, exactly as review_labels is
     * and for the same reason: these are .bss, so a failed derivation must not
     * leave the previous account's rows sitting here to be wiped by a caller
     * that this time never gets far enough to do it. */
    seedtool_zero(address_labels, sizeof(address_labels));
    screen_text("Addresses", "Deriving addresses...", NULL, NULL);
    if (seedtool_mainnet_addresses(mnemonic, passphrase, type, account, chain, ADDRESS_LIST_ROWS, addresses)
        != SEEDTOOL_OK) {
        seedtool_zero(addresses, sizeof(addresses));
        (void)acknowledge("Error", "Could not derive addresses", NULL);
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
    address_items[ADDRESS_SHOWN_ROWS + 1] = "Back";
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
static int browse_addresses(char* address_out, const size_t address_out_len, size_t* cursor)
{
    for (;;) {
        const int selected = choose_at("Addresses", address_items, ADDRESS_SHOWN_ROWS + 2, true, *cursor);
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
        if (selected >= 0) {
            *cursor = (size_t)selected;
        }
        if (selected >= 0 && selected < (int)ADDRESS_SHOWN_ROWS) {
            (void)snprintf(address_out, address_out_len, "%.*s", (int)(SEEDTOOL_MAX_ADDRESS_LEN - 1),
                addresses[selected]);
        }
        return selected < 0 || selected == (int)ADDRESS_SHOWN_ROWS + 1 ? -1 : selected;
    }
}

/* One address type's worth of the wallet viewer: its account key, in whichever
 * format was asked for, and its addresses. SLIP-132 defines no taproot version
 * prefix, so BIP86 never offers a format choice, only BIP84 does. */
static void show_type_menu(const char* mnemonic, const char* passphrase, const char* fphex,
    const seedtool_address_type_t type, const uint32_t account)
{
    const char* const title = type == SEEDTOOL_BIP84 ? "Native SegWit" : "Taproot";
    for (;;) {
        const char* items[] = { "Account key", "Descriptor", "Addresses", "Back" };
        const int selected = choose(title, items, 4, true);
        if (selected < 0 || selected == 3) {
            return;
        }
        if (selected == 0) {
            seedtool_key_format_t format = SEEDTOOL_XPUB;
            if (type == SEEDTOOL_BIP84) {
                const char* const formats[] = { "xpub", "zpub", "Back" };
                const int chosen = choose("Account key format", formats, 3, true);
                if (chosen < 0 || chosen == 2) {
                    continue;
                }
                format = chosen == 0 ? SEEDTOOL_XPUB : SEEDTOOL_ZPUB;
            }
            char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
            char origin[32];
            (void)snprintf(origin, sizeof(origin), "[%s/%u'/0'/%u']", fphex, (unsigned)type, (unsigned)account);
            if (seedtool_account_xpub(mnemonic, passphrase, type, account, format, xpub, sizeof(xpub))
                == SEEDTOOL_OK) {
                if (page_text(origin, xpub)) {
                    export_qr(mnemonic, passphrase, fphex, type, account, format);
                }
            } else {
                (void)acknowledge("Error", "Could not derive account key", NULL);
            }
            seedtool_zero(xpub, sizeof(xpub));
        } else if (selected == 1) {
            show_descriptor(mnemonic, passphrase, fphex, type, account);
        }
        if (selected != 2) {
            continue;
        }
        /* Which branch, asked the same way and in the same shape the account
         * key's xpub/zpub choice above is asked: the list itself is identical
         * either way, so the question belongs before it rather than as a mode
         * to toggle inside it. */
        const char* const branches[] = { "Receive", "Change", "Back" };
        const int branch = choose("Addresses", branches, 3, true);
        if (branch < 0 || branch == 2) {
            continue;
        }
        const seedtool_chain_t chain = branch == 0 ? SEEDTOOL_RECEIVE : SEEDTOOL_CHANGE;
        if (derive_addresses(mnemonic, passphrase, type, account, chain)) {
            /* Loops back to the address list itself after each address's QR,
             * rather than out to this menu: picking another address is the
             * common next step, not re-choosing "Addresses" again - and,
             * since derive_addresses ran once above rather than on every pass
             * through this loop, coming straight back from one address's QR
             * no longer costs the whole derivation again. Only backing out of
             * the list (or a timeout) reaches the outer loop. `cursor` lives
             * outside this loop so the list reopens wherever it was left,
             * rather than back at address 0 every time. */
            size_t cursor = 0;
            for (;;) {
                char address[SEEDTOOL_MAX_ADDRESS_LEN] = { 0 };
                const int index = browse_addresses(address, sizeof(address), &cursor);
                if (index < 0) {
                    seedtool_zero(address, sizeof(address));
                    break;
                }
                char path[32];
                (void)snprintf(path, sizeof(path), "m/%u'/0'/%u'/%u/%u", (unsigned)type, (unsigned)account,
                    (unsigned)chain, (unsigned)index);
                if (page_text(path, address)) {
                    show_address_qr(path, address);
                }
                seedtool_zero(address, sizeof(address));
            }
            seedtool_zero(addresses, sizeof(addresses));
            seedtool_zero(address_labels, sizeof(address_labels));
        }
    }
}

/* One word per screen, stepped the same way show_qr steps between values:
 * L/R moves, anything else (BOTH, timeout) leaves. A plate punched from this
 * already restores today with zero new code, via "Enter word numbers". */
static void show_stackbit(const char* mnemonic)
{
    uint16_t numbers[24];
    size_t count = 0;
    if (seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &count) != SEEDTOOL_OK) {
        (void)acknowledge("Error", "Could not compute", "word numbers");
        return;
    }
    const char* const layouts[] = { "Simple grid", "Physical layout", "Back" };
    const int layout = choose("Stackbit 1248", layouts, 3, true);
    if (layout < 0 || layout == 2) {
        seedtool_zero(numbers, sizeof(numbers));
        return;
    }
    size_t selected = 0;
    for (;;) {
        char footer[16];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(selected + 1), (unsigned)count);
        const char* const word = seedtool_word(numbers[selected] - 1);
        if (layout == 0) {
            seedtool_display_stackbit_screen("Stackbit 1248", numbers[selected], word, footer);
        } else {
            seedtool_display_stackbit_physical_screen("Stackbit 1248", numbers[selected], word, footer);
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
 * show_backup_menu's own callers, which don't chain, just discard it. */
static bool show_numbered_list(const char* mnemonic, const bool show_words)
{
    uint16_t numbers[24];
    size_t count = 0;
    if (seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &count) != SEEDTOOL_OK) {
        (void)acknowledge("Error", "Could not compute", "word numbers");
        return false;
    }
    const size_t pages = (count + 3) / 4;
    size_t page = 0;
    bool advanced;
    /* Hoisted for the same reason as page_text's: every exit already wiped
     * `numbers`, the dictionary positions, while leaving the words those
     * positions spell rendered in full underneath. */
    char lines[4][24] = { { 0 } };
    for (;;) {
        memset(lines, 0, sizeof(lines));
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
        char footer[16];
        (void)snprintf(footer, sizeof(footer), "%u/%u", (unsigned)(page + 1), (unsigned)pages);
        screen_text4(show_words ? "BIP39 words" : "BIP39 word numbers", lines[0], lines[1], lines[2], lines[3],
            footer);
        switch (wait_key()) {
        case KEY_SELECT:
            advanced = true;
            goto done;
        case KEY_NEXT:
            if (page + 1 >= pages) {
                advanced = true;
                goto done;
            }
            ++page;
            break;
        case KEY_PREV:
            if (!page) {
                advanced = false;
                goto done;
            }
            --page;
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

/* Entering this screen reaches the whole seed, so the warning is far starker
 * than the account-key QR's: that one only ever exposes future addresses,
 * this one is every key the mnemonic can ever derive. There is no camera to
 * scan the result back with, so tools/origo_verify.py inspect prints the same
 * payload for an independent check instead. */
/* Index 0 is the full code, seen at its natural size first; index 1 is the
 * region map (that same code, with the boundaries and labels each zoomed
 * tile beyond it will use); 2..regions+1 are Krux-style "Zoomed Region"
 * tiles, stepped sideways the same way show_qr steps between values, so the
 * carousel convention stays one shape everywhere a QR is shown. */
static void export_seed_qr(const char* mnemonic)
{
    if (!acknowledge("Compact SeedQR", "Encodes your ENTIRE seed", "A photo = total loss of funds")) {
        return;
    }
    uint8_t entropy[SEEDTOOL_HASH_LEN] = { 0 };
    size_t len = 0;
    if (seedtool_mnemonic_entropy(mnemonic, entropy, sizeof(entropy), &len) == SEEDTOOL_OK) {
        const size_t regions = seedtool_render_qr_bytes_regions(len);
        const size_t steps = regions + 2;
        size_t selected = 0;
        for (;;) {
            const bool ok = selected == 0
                ? seedtool_display_qr_bytes("Compact SeedQR", entropy, len)
                : selected == 1 ? seedtool_display_qr_bytes_map("Compact SeedQR", entropy, len)
                                : seedtool_display_qr_bytes_region("Compact SeedQR", entropy, len, selected - 2);
            if (!ok) {
                (void)acknowledge("Too long for a QR", "Compact SeedQR", "Read it as text instead");
                break;
            }
            switch (wait_key()) {
            case KEY_PREV:
                selected = (selected + steps - 1) % steps;
                break;
            case KEY_NEXT:
                selected = (selected + 1) % steps;
                break;
            case KEY_REDRAW:
                break;
            default:
                goto done;
            }
        }
    } else {
        (void)acknowledge("Error", "Could not derive entropy", NULL);
    }
done:
    seedtool_zero(entropy, sizeof(entropy));
}

static void show_backup_menu(const char* mnemonic)
{
    for (;;) {
        const char* items[] = { "Words", "Numbers", "Stackbit 1248", "Compact SeedQR", "Back" };
        const int selected = choose("Backup", items, 5, true);
        if (selected < 0 || selected == 4) {
            return;
        }
        if (selected == 0) {
            (void)show_numbered_list(mnemonic, true);
        } else if (selected == 1) {
            (void)show_numbered_list(mnemonic, false);
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
 * Gathered here they read as what they are: the parameters of the derivation,
 * editable in any order, none of them a place you pass *through*.
 *
 * `fp`/`fphex` come in by pointer because the passphrase is one of the two
 * inputs to the master fingerprint: change it and the cached fingerprint the
 * Wallet menu shows is stale, so it is re-derived here rather than left to
 * disagree with what the device is now deriving from.
 *
 * Follows show_settings_menu's shape exactly - live-formatted labels rebuilt
 * each pass, a row either toggling in place or opening a dedicated editor and
 * returning. */
static void show_customize_menu(const char* mnemonic, uint32_t* account, seedtool_address_type_t* type,
    char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1], uint8_t fp[4], char fphex[9])
{
    for (;;) {
        char account_item[16];
        (void)snprintf(account_item, sizeof(account_item), "Account: %u", (unsigned)*account);
        const char* const type_item = *type == SEEDTOOL_BIP84 ? "Type: Native SegWit" : "Type: Taproot";
        /* Says whether one is set, never anything about what it is. */
        const char* const passphrase_item = passphrase[0] ? "Passphrase: session only" : "Passphrase: none";
        const char* items[] = { account_item, type_item, passphrase_item, "Back" };
        const int selected = choose("Customize", items, 4, true);
        if (selected < 0 || selected == 3) {
            return;
        }
        if (selected == 0) {
            uint32_t chosen = *account;
            if (enter_account(&chosen) == 1) {
                *account = chosen;
            }
        } else if (selected == 1) {
            /* Toggled in place rather than through a chooser: there are two
             * types and the row already names the one in force, so a submenu
             * would be a screen to say what the label says. */
            *type = *type == SEEDTOOL_BIP84 ? SEEDTOOL_BIP86 : SEEDTOOL_BIP84;
        } else if (edit_session_passphrase(passphrase)) {
            if (seedtool_master_fingerprint(mnemonic, passphrase, fp) != SEEDTOOL_OK) {
                (void)acknowledge("Error", "Derivation failed", NULL);
            } else {
                hexstr(fp, 4, fphex);
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
     * decided before the wallet existed and could not be revisited after. What
     * is in force is always on Customize's own row and beside the master
     * fingerprint, so "none" is stated rather than merely defaulted to. */
    if (seedtool_master_fingerprint(mnemonic, passphrase, fp) != SEEDTOOL_OK) {
        (void)acknowledge("Error", "Derivation failed", NULL);
        goto done;
    }
    hexstr(fp, sizeof(fp), fphex);

    /* m/type'/0'/account' and the passphrase above it: all three live for the
     * whole wallet-viewing session, not one visit to a type's screens, so
     * checking account 2 under both types means setting it once - not
     * resetting on the way from one to the other. They are edited together in
     * Customize; the row below names the type in force so the menu says what
     * it will show before it is opened. Neither Master fingerprint (always the
     * root's, account- and type-independent) nor Backup (about the mnemonic
     * itself, not a derivation) reads account or type. */
    uint32_t account = 0;
    seedtool_address_type_t type = SEEDTOOL_BIP84;
    for (;;) {
        const char* const view_item = type == SEEDTOOL_BIP84 ? "Native SegWit" : "Taproot";
        const char* menu[] = { "Master fingerprint", "Customize", view_item, "Backup", "Done / erase" };
        const int selected = choose("Wallet", menu, sizeof(menu) / sizeof(menu[0]), true);
        if (selected < 0 || selected == 4) {
            break;
        }
        if (selected == 0) {
            (void)acknowledge(
                "Master fingerprint", fphex, passphrase[0] ? "Passphrase: session only" : "Passphrase: none");
        } else if (selected == 1) {
            show_customize_menu(mnemonic, &account, &type, passphrase, fp, fphex);
        } else if (selected == 2) {
            show_type_menu(mnemonic, passphrase, fphex, type, account);
        } else {
            show_backup_menu(mnemonic);
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
            const int result = enter_word(index + 1, count, typed, sizeof(typed));
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
            (void)acknowledge("Backup confirmed", "Words matched", NULL);
            return 1;
        }
        if (!acknowledge("Word doesn't match", "Check your backup", "Try again")) {
            seedtool_zero(numbers, sizeof(numbers));
            return 0;
        }
    }
}

static void show_generated(seedtool_generated_t* generated)
{
    char hash[65];
    hexstr(generated->hash, sizeof(generated->hash), hash);
    if (page_text("Canonical transcript", generated->transcript) && page_text("SHA256", hash)) {
        /* Backing out of the quiz's first word (confirm_backup's 0) shows
         * the words again rather than falling through, so back always steps
         * back one stage instead of skipping past confirmation into the
         * wallet's passphrase prompt. */
        while (show_numbered_list(generated->mnemonic, true)) {
            /* The word list is left by paging forward off its last page - the
             * same press that meant "next page" for the whole list - so
             * without this screen the quiz's keyboard simply appeared, with
             * no way to tell "show me more" from "I have written these down"
             * and no warning that the words were about to leave the screen.
             * The quiz's own "word N of M" title says which word it wants but
             * never why it is asking, so the count is named here instead,
             * before the words go away rather than after.
             *
             * `intro` is sized for the format's worst case rather than its
             * real one: the counts are 4/12 or 8/24, but %u lets the compiler
             * assume ten digits apiece, and -Wformat-truncation is an error in
             * the firmware build even though the host build lets it pass. */
            char intro[48];
            (void)snprintf(intro, sizeof(intro), "Retype %u of the %u words", (unsigned)(generated->words / 3),
                (unsigned)generated->words);
            const seedtool_key_t intro_key = confirm("Confirm backup", intro, "Have your backup ready");
            if (intro_key == KEY_TIMEOUT) {
                /* Not `continue`: that would repaint the whole mnemonic on
                 * the way out of a session that has already expired. */
                break;
            }
            if (intro_key != KEY_SELECT) {
                continue; /* Back: the words again, one stage, as everywhere else. */
            }
            const int outcome = confirm_backup(generated->mnemonic, generated->words);
            if (outcome < 0) {
                break;
            }
            if (outcome == 1) {
                show_wallet_data(generated->mnemonic);
                break;
            }
        }
    }
    seedtool_zero(hash, sizeof(hash));
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
        if (!dice_confirm(names[source], needed, hint, &empty)) {
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
        if (poor) {
            proceed = acknowledge("Poor entropy!", bits_line, "Proceed anyway?");
        }
        if (proceed && pattern) {
            proceed = acknowledge("Pattern detected!", "Proceed anyway?", NULL);
        }
        if (proceed && !poor && !pattern) {
            /* The positive case: the bar's outline goes green, the one point in
             * the run where seedtool_progress_t.complete is ever true - but the
             * verdict is also spelled out in the question itself, not left to
             * the border colour alone, since not every reader (or every
             * lighting condition) tells a green outline from a dim one at a
             * glance. */
            const seedtool_progress_t complete = { .rolls_pct = 100, .entropy_pct = 100, .warn = false, .complete = true };
            proceed = dice_confirm(names[source], bits_line, "Looks good - generate?", &complete);
        }
        if (proceed) {
            break;
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

/* Returns whether a seed was actually generated and carried all the way
 * through the wallet viewer to Done/erase (or a timeout) - as opposed to the
 * reader backing out of the source or length picker before ever starting.
 * show_new_seed_menu uses this to tell "done, go all the way home" apart from
 * plain "back one level". */
static bool create_seed(void)
{
    for (;;) {
        const char* sources[] = { "D6 dice", "D20 dice", "Coin flips", "Cards", "Back" };
        const int source = choose("Entropy source", sources, sizeof(sources) / sizeof(sources[0]), true);
        if (source < 0 || source == 4) {
            return false;
        }
        for (;;) {
            const char* lengths[] = { "12 words", "24 words", "Back" };
            const int length = choose("Seed length", lengths, sizeof(lengths) / sizeof(lengths[0]), true);
            if (length < 0) {
                return false;
            }
            if (length == 2) {
                break;
            }
            /* 24-word Cards can't reach 256 bits without replacement (one
             * deck tops out around 225 bits) - SEEDTOOL_CARDS_REPLACE draws
             * the same deck, but with the card returned and the deck
             * reshuffled after every draw, a genuine 52-sided die instead. */
            const int collect_source = length && source == SEEDTOOL_CARDS ? SEEDTOOL_CARDS_REPLACE : source;
            if (collect_entropy(collect_source, length ? 24 : 12) != 0) {
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
    for (;;) {
        const char* lengths[] = { "11 words + 7 coins", "23 words + 3 coins", "Back" };
        const int selected = choose("Complete checksum", lengths, 3, true);
        if (selected < 0 || selected == 2) {
            return false;
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
            const int result
                = enter_coin_flip("Coin flip", (unsigned)(i + 1), (unsigned)bits_count, &bit, flips, NULL);
            seedtool_zero(flips, sizeof(flips));
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
    for (;;) {
        const char* items[] = { "From entropy", "Complete checksum", "Back" };
        const int selected = choose("New Seed", items, 3, true);
        if (selected < 0 || selected == 2) {
            return;
        }
        /* A seed that made it all the way to Done/erase (or a timeout) closes
         * this menu too, straight back to the Origo/Home menu, rather than
         * reopening "New Seed" - that reopening was the actual bug: pressing
         * Done/erase landed back inside New Seed instead of at Home. Backing
         * out of the source/length picker before anything was generated
         * keeps the old "one level up" behaviour. */
        if (selected == 0 ? create_seed() : complete_checksum()) {
            return;
        }
    }
}

static void restore_seed(void)
{
    for (;;) {
        const char* lengths[] = { "12 words", "24 words", "Back" };
        const int selected = choose("Restore mnemonic", lengths, 3, true);
        if (selected < 0 || selected == 2) {
            return;
        }
        char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
        /* restore_mnemonic's review step only ever returns 1 with a checksum
         * that already passed - a reader who fixes a word doesn't lose the
         * other 11 or 23, and one who can't gets to keep trying instead of
         * being dropped straight back to "12 words / 24 words / Back". */
        const int outcome = restore_mnemonic(selected ? 24 : 12, mnemonic, sizeof(mnemonic));
        if (outcome == 1 && acknowledge("Checksum valid", "BIP39 English", "Derivation unlocked")) {
            show_wallet_data(mnemonic);
        }
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
    for (;;) {
        char orientation_item[32], brightness_fraction[8], brightness_item[24];
        (void)snprintf(orientation_item, sizeof(orientation_item), "Flip Orientation: %s",
            orientation_flipped ? "On" : "Off");
        format_brightness(brightness_fraction, sizeof(brightness_fraction));
        (void)snprintf(brightness_item, sizeof(brightness_item), "Brightness: %s", brightness_fraction);
        const char* items[] = { orientation_item, brightness_item, "About", "Back" };
        const int selected = choose("Settings", items, 4, true);
        if (selected < 0 || selected == 3) {
            return;
        }
        if (selected == 0) {
            orientation_flipped = !orientation_flipped;
            seedtool_display_set_orientation(orientation_flipped);
        } else if (selected == 1) {
            show_brightness();
        } else {
            (void)page_text("Safety",
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

    for (;;) {
        const char* menu[] = { "New Seed", "Restore Seed", "Settings" };
        const int selected = choose("Origo", menu, sizeof(menu) / sizeof(menu[0]), false);
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
