/* Mnemonic entry: one word by letters or by number, a whole mnemonic, the
 * review list that fixes any word of it, and the passphrase keyboard. */
#include "app_internal.h"

#include <stdio.h>
#include <string.h>

/* One BIP39 word into `output`. Returns false when the reader deleted back out
 * of it. Only letters that still lead to a word are enabled, and ten or fewer
 * candidates are listed outright.
 *
 * `allowed` narrows the words, NULL for all of them. Every enabled letter leads
 * to an allowed word, so the stem never runs out of candidates. */
bool enter_word(const size_t position, const size_t total, const seedtool_wordset_t* allowed,
    char output[SEEDTOOL_MAX_WORD_LEN + 1])
{
    char stem[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
    size_t stem_len = 0;
    /* The cursor opens at the centre and then keeps its place between letters,
     * so a word is not retyped from the far corner every time. */
    size_t selected = seedtool_layout_center(word_layout());
    char title[24];
    (void)snprintf(title, sizeof(title), "Word %u/%u", (unsigned)position, (unsigned)total);

    for (;;) {
        uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
        const size_t matches
            = seedtool_words_with_prefix_in(allowed, stem, stem_len, candidates, SEEDTOOL_MAX_WORD_CHOICES);
        bool erase = false;
        seedtool_require(matches > 0);
        if (matches <= SEEDTOOL_MAX_WORD_CHOICES) {
            /* Ten or fewer candidates: list them, with the stem in the title. */
            const char* items[SEEDTOOL_MAX_WORD_CHOICES + 1];
            char listing[sizeof(title) + SEEDTOOL_MAX_WORD_LEN + 3];
            for (size_t i = 0; i < matches; ++i) {
                items[i] = seedtool_word(candidates[i]);
            }
            /* The arrow deletes a letter, or leaves the word from an empty
             * stem. */
            (void)snprintf(listing, sizeof(listing), "%s  %s", title, stem_len ? stem : "-");
            const int chosen = choose_menu(listing, items, matches);
            seedtool_zero(listing, sizeof(listing));
            if (chosen != NAV_BACK) {
                strcpy(output, items[chosen]);
                seedtool_zero(stem, sizeof(stem));
                seedtool_zero(candidates, sizeof(candidates));
                return true;
            }
            erase = true;
        } else {
            /* Too many candidates to list: narrow the stem one letter at a time. */
            bool letters[SEEDTOOL_LETTERS] = { false };
            bool enabled[WORD_KEYS] = { false };
            (void)seedtool_next_letters_in(allowed, stem, stem_len, letters);
            for (size_t i = 0; i < WORD_KEYS; ++i) {
                const char key = seedtool_layout_key(word_layout(), i);
                enabled[i] = key == SEEDTOOL_KEY_BACKSPACE || letters[key - 'a'];
            }
            selected = nearest_enabled(enabled, WORD_KEYS, selected);
            bool picked = false;
            while (!picked) {
                seedtool_render_keyboard(title, stem_len ? stem : "-", word_layout(), enabled, selected, position, total);
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
                default:
                    break;
                }
            }
            const char key = seedtool_layout_key(word_layout(), selected);
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
                return false;
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
static bool enter_word_number(const size_t position, const size_t total, const seedtool_wordset_t* allowed,
    char output[SEEDTOOL_MAX_WORD_LEN + 1])
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

        const seedtool_key_t key = wait_key();
        if (key == KEY_REDRAW) {
            continue;
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
                return false;
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
        const int answer = word ? nav_screen(title, word, counted, "Use this word", false, NULL) : NAV_BACK;
        seedtool_zero(counted, sizeof(counted));
        if (answer == NAV_CONFIRM) {
            strcpy(output, word);
            seedtool_zero(digits, sizeof(digits));
            seedtool_zero(shown, sizeof(shown));
            return true;
        }
        /* Back from the confirmation: a number completed by its fourth digit
         * reopens that digit; one ended with OK returns to the field as it was. */
        if (typed_last) {
            digits[--at] = '\0';
        }
        rebuild = true;
    }
}

/* A number of up to `max_digits` digits on the word-number keypad, every digit
 * reachable at any length, since every prefix is already in range. Returns true
 * with `*value` set on Accept, false when the reader backed out of the first
 * digit. */
bool enter_number(const char* title, const size_t max_digits, uint32_t* value)
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
                                                       : digits_len < max_digits;
        }
        /* With the digits full, the cursor goes to Accept. */
        selected
            = !enabled[selected] && digits_len ? accept_index : nearest_enabled(enabled, WORD_NUMBER_KEYS, selected);
        bool picked = false;
        while (!picked) {
            seedtool_render_keyboard(title, digits_len ? digits : "-", WORD_NUMBER_LAYOUT, enabled, selected, 0, 0);
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
            default:
                break;
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
            return true;
        }
        if (pressed == SEEDTOOL_KEY_BACKSPACE) {
            if (!digits_len) {
                seedtool_zero(digits, sizeof(digits));
                return false;
            }
            digits[--digits_len] = '\0';
        } else if (digits_len < max_digits) {
            digits[digits_len++] = pressed;
            digits[digits_len] = '\0';
        }
    }
}

/* words[0..count) joined with single spaces: the string
 * seedtool_validate_mnemonic sees. */
bool join_words(
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

/* Fills `words` and returns true when a whole mnemonic was entered, false when
 * the reader backed out of the first word or the method menu. `by_number`, if
 * not NULL, reports the entry method, which re-entering a word reuses. */
bool enter_mnemonic_words(const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number)
{
    const char* methods[] = { "Type the letters", "Enter word numbers" };
    const int method = choose_menu("Word entry", methods, ARRAY_LEN(methods));
    if (method == NAV_BACK) {
        return false;
    }
    if (by_number) {
        *by_number = method != 0;
    }
    bool entered = true;
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
            seedtool_require(join_words(words, count - 1, prefix, sizeof(prefix))
                && seedtool_final_word_candidates(prefix, &allowed) == SEEDTOOL_OK);
            seedtool_zero(prefix, sizeof(prefix));
            filter = &allowed;
        }
        if (method ? enter_word_number(index + 1, count, filter, words[index])
                   : enter_word(index + 1, count, filter, words[index])) {
            ++index;
            continue;
        }
        /* Deleting past the start of a word steps back to the previous one,
         * and past the first word returns to the method menu. */
        if (!index) {
            entered = false;
            break;
        }
        words[--index][0] = '\0';
    }
    /* The set says something about the words before it. */
    seedtool_zero(&allowed, sizeof(allowed));
    return entered;
}

static char review_labels[24][32];
static const char* review_items[24];

/* The entered words as a list, so any one can be fixed without retyping the
 * rest. Returns true on Continue, false on Back.
 *
 * With `mnemonic`, the words are a whole mnemonic: they are rejoined into it
 * after each edit, Continue is dim while the checksum fails, and the last word
 * re-enters under its checksum filter. Without, they are complete_checksum's
 * 11 or 23 words, which no checksum applies to yet. */
static bool review_words(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, const bool by_number,
    char* mnemonic, const size_t mnemonic_len)
{
    /* Opens on the confirm: after typing the words the reader is more often
     * done than not. */
    size_t cursor = SEEDTOOL_NAV_CONFIRM;
    bool confirmed;
    /* The rows are the mnemonic, in .bss: cleared on the way in, so a 12-word
     * restore shows none of an earlier 24-word one, and on the way out. */
    seedtool_zero(review_labels, sizeof(review_labels));
    for (;;) {
        bool valid = true;
        if (mnemonic) {
            seedtool_require(join_words(words, count, mnemonic, mnemonic_len));
            valid = seedtool_validate_mnemonic(mnemonic, NULL) == SEEDTOOL_OK;
        }
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
        if (selected == NAV_BACK || selected == NAV_CONFIRM) {
            confirmed = selected == NAV_CONFIRM;
            break;
        }
        char word[SEEDTOOL_MAX_WORD_LEN + 1] = { 0 };
        /* The last word of a whole mnemonic re-enters narrowed to what can end
         * the current words. If the set cannot be built it is entered
         * unnarrowed: this screen must never block a fix. */
        seedtool_wordset_t allowed;
        const seedtool_wordset_t* filter = NULL;
        if (mnemonic && (size_t)selected + 1 == count) {
            char prefix[SEEDTOOL_MAX_MNEMONIC_LEN + 1] = { 0 };
            if (join_words(words, count - 1, prefix, sizeof(prefix))
                && seedtool_final_word_candidates(prefix, &allowed) == SEEDTOOL_OK) {
                filter = &allowed;
            }
            seedtool_zero(prefix, sizeof(prefix));
        }
        /* Backing out of the word leaves it as it was. */
        if (by_number ? enter_word_number(selected + 1, count, filter, word)
                      : enter_word(selected + 1, count, filter, word)) {
            strcpy(words[selected], word);
        }
        seedtool_zero(&allowed, sizeof(allowed));
        seedtool_zero(word, sizeof(word));
    }
    seedtool_zero(review_labels, sizeof(review_labels));
    return confirmed;
}

/* The words stage of a checksum completion: the list with the entry screen
 * behind it, so back steps one screen at a time. Returns true once the reader
 * comes forward, false when they step back past entry. */
bool revisit_prefix(char words[][SEEDTOOL_MAX_WORD_LEN + 1], const size_t count, bool* const by_number)
{
    for (;;) {
        if (review_words(words, count, *by_number, NULL, 0)) {
            return true;
        }
        if (!enter_mnemonic_words(count, words, by_number)) {
            return false;
        }
    }
}

/* Entry then review, for a mnemonic meant to be whole. The last word is entered
 * under its checksum filter, so a completed entry always validates; the review
 * still gates Continue on it. The review's arrow reopens entry; `resume`
 * starts on the review, for a caller handing the words back. */
bool restore_mnemonic(const size_t count, char words[][SEEDTOOL_MAX_WORD_LEN + 1], bool* const by_number,
    char* mnemonic, const size_t mnemonic_len, bool resume)
{
    for (;;) {
        if (!resume && !enter_mnemonic_words(count, words, by_number)) {
            return false;
        }
        resume = false;
        if (review_words(words, count, *by_number, mnemonic, mnemonic_len)) {
            return true;
        }
    }
}

/* The passphrase keyboard, editing what `output` already holds. Returns true
 * on Accept, false on backspace with nothing left to delete. */
bool enter_passphrase_once(char* output, const size_t output_len)
{
    size_t used = strlen(output), page = 0;
    size_t selected = seedtool_layout_center(passphrase_layout(0));
    for (;;) {
        const char* const layout = passphrase_layout(page);
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
        default:
            continue;
        }
        const char pressed = seedtool_layout_key(layout, selected);
        if (pressed == SEEDTOOL_KEY_ACCEPT) {
            return true;
        }
        if (pressed == SEEDTOOL_KEY_PAGE) {
            page = (page + 1) % PASSPHRASE_PAGES;
            /* A new page is a new keyboard, so the cursor starts from its centre
             * rather than from wherever the page key happened to sit. */
            selected = seedtool_layout_center(passphrase_layout(page));
        } else if (pressed == SEEDTOOL_KEY_BACKSPACE) {
            if (!used) {
                return false;
            }
            output[--used] = '\0';
        } else if (used + 1 < output_len) {
            output[used++] = pressed;
            output[used] = '\0';
        }
    }
}
