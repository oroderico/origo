/* What the app's modules share: the flow screens are split by what they deal
 * with (app_ui.c, app_words.c, app_wallet.c, app_seed.c), and these are the
 * helpers, limits and state they call across that split. */
#ifndef APP_INTERNAL_H_
#define APP_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seedtool_app.h"
#include "seedtool_bbqr.h"
#include "seedtool_core.h"
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

/* The first ADDRESS_SHOWN_ROWS addresses are derived and listed; "Go to index"
 * derives any other up to SEEDTOOL_MAX_ADDRESS_INDEX on its own. */
#define ADDRESS_SHOWN_ROWS 50
#define ADDRESS_LABEL_LEN (8 + SEEDTOOL_MAX_ADDRESS_LEN)
_Static_assert(ADDRESS_SHOWN_ROWS <= SEEDTOOL_MAX_ADDRESS_INDEX + 1, "more address rows are listed than exist");

/* The two buttons sit one above the other, so the left one (KEY_PREV) reads as
 * "up". Spelled out because the fonts have no arrow glyph. */
#define NAV_FOOTER "Up/Down move   BOTH select"
/* The digit field keeps a footer: up and down change the digit, so the chrome's
 * two controls have nothing to attach to. */
#define DIGIT_FOOTER "Up/Down set   BOTH take"
/* A coin flip's chord undoes rather than selects, and nothing else on the
 * screen says so. */
#define COIN_FOOTER "BOTH undoes the last flip"

/* Word entry keyboard: the letters plus backspace, in either arrangement. */
#define WORD_KEYS (SEEDTOOL_LETTERS + 1)

/* Word number keyboard, for a backup that records numbers rather than words. */
#define WORD_NUMBER_LAYOUT SEEDTOOL_WORD_NUMBER_LAYOUT
#define WORD_NUMBER_KEYS (SEEDTOOL_DIGITS + 2)

/* The account index uses the word-number keypad. Three digits reach
 * SEEDTOOL_MAX_ACCOUNT_INDEX, so every digit is reachable at any length. */
#define ACCOUNT_DIGITS 3

/* "Go to index", sized for SEEDTOOL_MAX_ADDRESS_INDEX: four digits reach it,
 * so every digit is reachable at any length. */
#define ADDRESS_INDEX_DIGITS 4
_Static_assert(ADDRESS_INDEX_DIGITS == 4 && SEEDTOOL_MAX_ADDRESS_INDEX == 9999,
    "the address-index keypad width and the derived address range must be changed together");

/* A SeedPicker ticket, T1..T342, on the same keypad: three digits reach it. */
#define TICKET_DIGITS 3
_Static_assert(TICKET_DIGITS == 3 && SEEDTOOL_TICKETS >= 100 && SEEDTOOL_TICKETS <= 999,
    "the ticket keypad width and the ticket count must be changed together");

/* enter_number's buffer, sized for the widest of its uses. */
#define NUMBER_MAX_DIGITS 4
_Static_assert(ACCOUNT_DIGITS <= NUMBER_MAX_DIGITS && ADDRESS_INDEX_DIGITS <= NUMBER_MAX_DIGITS
        && TICKET_DIGITS <= NUMBER_MAX_DIGITS,
    "enter_number holds NUMBER_MAX_DIGITS digits");

/* The word-number field draws one box per digit, and the renderer bounds how
 * many of those fit across the display. Widening a word number without the
 * boxes to show it would run the row off both edges, which clips silently. */
_Static_assert(SEEDTOOL_MAX_WORD_DIGITS <= SEEDTOOL_DIGIT_BOXES_MAX,
    "a word number has more digits than the digit field can draw boxes for");

#define PASSPHRASE_PAGES SEEDTOOL_PASSPHRASE_PAGES


extern uint64_t last_action;

/* The chord hint shows until the chord has been used once. */
extern bool chord_learned;

/* Display settings live only in RAM and reset every boot. */
extern bool orientation_flipped;
extern unsigned backlight_level;

/* Letter keyboards in QWERTY rather than alphabetical rows. Settings opens
 * from the Origo and wallet menus, never from inside a flow: a passphrase is
 * typed and confirmed in one sitting, so never half on each arrangement. */
extern bool qwerty_keys;

/* choose_nav's outcomes other than an item index. page_key adds NAV_STAY. */
#define NAV_STAY (-1)
#define NAV_BACK (-2)
#define NAV_CONFIRM (-3)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* app_ui.c */
void seedtool_require(const bool condition);
bool show_qr(const bool drawn);
void screen_text(const char* title, const char* line1, const char* line2, const char* footer);
seedtool_key_t wait_key(void);
seedtool_key_t wait_key_or_tick(const uint32_t frame_ms, bool* const ticked);
int nav_screen(const char* title, const char* one, const char* two, const char* label, const bool start_on_back);
bool nav_acknowledge(const char* title, const char* one, const char* two, const char* label, const bool start_on_back);
void notice(const char* title, const char* one, const char* two);
int choose_nav(const char* title, const char* fingerprint, const char* const* items, const size_t count,
    const char* confirm, const bool confirm_enabled, const bool back, size_t* const cursor);
size_t page_step(const size_t position, const size_t pages, const bool forward, const bool confirmable);
size_t page_shown(const size_t position, const size_t pages);
int page_key(size_t* const position, const size_t pages, const bool confirmable);
seedtool_nav_t page_nav(const size_t position, const size_t pages, const char* counter, const bool confirmable,
    const seedtool_confirm_t style);
int choose_menu_at(const char* title, const char* const* items, const size_t count, size_t* const cursor);
int choose_menu(const char* title, const char* const* items, const size_t count);
bool enter_value(const char* title, const unsigned position, const unsigned total, const unsigned min,
    const unsigned max, unsigned* value);
bool enter_coin_flip(
    const char* title, const unsigned position, const unsigned total, unsigned* bit, const char* history);
void hexstr(const uint8_t* bytes, const size_t len, char* output);
bool page_text_impl(const char* title, const char* text, const bool confirmable, const bool grouped);
bool page_text(const char* title, const char* text);
void page_read(const char* title, const char* text);
size_t step_key(const bool* enabled, const size_t count, size_t index, const bool forward);
size_t nearest_enabled(const bool* enabled, const size_t count, const size_t index);
size_t layout_key_index(const char* layout, const char target);
const char* word_layout(void);
const char* passphrase_layout(const size_t page);

/* app_words.c */
bool enter_word(const size_t position, const size_t total, const seedtool_wordset_t* allowed,
    char output[SEEDTOOL_MAX_WORD_LEN + 1]);
bool enter_number(
    const char* title, const size_t max_digits, const uint32_t min, const uint32_t max, uint32_t* value);
bool join_words(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, char* mnemonic, const size_t mnemonic_len);
bool enter_mnemonic_words(const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number);
bool revisit_prefix(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, bool* const by_number);
bool restore_mnemonic(const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number,
    char* mnemonic, const size_t mnemonic_len, bool resume);
bool enter_passphrase_once(char* output, const size_t output_len);

/* app_wallet.c */
void show_wallet_data(const char* mnemonic);
bool review_backup_and_show_wallet(const char* mnemonic, const size_t words);

/* seedtool_app.c */
void show_settings_menu(void);

/* app_seed.c */
void show_new_seed_menu(void);
void restore_seed(void);

#endif
