/* The wallet a mnemonic opens: derivation settings, account keys and the
 * descriptor, addresses, the backup views and the backup quiz. */
#include "app_internal.h"

#include <stdio.h>
#include <string.h>

/* What a wallet session derives from: the mnemonic it opened with, the
 * passphrase, type and account set in Derivation, and the master key and
 * fingerprint those give. The key is derived once per passphrase rather than
 * per screen, since PBKDF2 is the slow step; it is no more secret than the
 * mnemonic beside it. Zeroed as one when the session ends. */
typedef struct {
    const char* mnemonic;
    char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1];
    seedtool_root_t root;
    char fphex[9];
    seedtool_address_type_t type;
    uint32_t account;
} wallet_session_t;

/* Derives the master key and fingerprint for the passphrase now set. */
static bool derive_root(wallet_session_t* const session)
{
    uint8_t fp[4];
    const bool ok = seedtool_root_from_mnemonic(session->mnemonic, session->passphrase, &session->root) == SEEDTOOL_OK
        && seedtool_master_fingerprint(&session->root, fp) == SEEDTOOL_OK;
    if (ok) {
        hexstr(fp, sizeof(fp), session->fphex);
    }
    seedtool_zero(fp, sizeof(fp));
    return ok;
}

/* The account key carries its key origin, so a scan does not have to be told
 * the derivation path afterwards. The account component's "999" is
 * SEEDTOOL_MAX_ACCOUNT_INDEX's own width, not a placeholder. */
#define ACCOUNT_KEY_LEN (sizeof("[00000000/84'/0'/999']") + SEEDTOOL_MAX_XPUB_LEN)

/* A BIP380 descriptor: prefix, origin, xpub, BIP389's "<0;1>" multipath step,
 * the wildcard, "#" and 8 checksum characters. "wpkh" is the longest prefix
 * used. */
#define DESCRIPTOR_LEN (sizeof("wpkh([00000000/84'/0'/999']/<0;1>/*)#12345678") + SEEDTOOL_MAX_XPUB_LEN)

/* The longest value ever handed to show_account_key_qr: the descriptor. */
#define BBQR_MAX_VALUE_LEN ((ACCOUNT_KEY_LEN > DESCRIPTOR_LEN ? ACCOUNT_KEY_LEN : DESCRIPTOR_LEN) - 1)
#define BBQR_FRAME_LEN (SEEDTOOL_BBQR_HEADER_LEN + (BBQR_MAX_VALUE_LEN * 8 + 4) / 5 + 1)

/* Frames advance on a timer only; up and down move between the screen's two
 * controls, as on every QR. A reader collects parts across repeats, so a frame
 * the camera misses comes round again. */
#define BBQR_FRAME_INTERVAL_MS 1000

/* Frames are capped at version 3: 4px modules on this 135px display, where
 * versions 4-6 give 3px and 1-2 double the frame count. */
#define BBQR_FRAME_MAX_VERSION 3

/* One key on a QR screen's two controls, the arrow and the sun: up and down
 * move between them, the chord on the sun cycles the shade. Returns true for
 * the chord on the arrow. */
static bool qr_key(const seedtool_key_t key, size_t* const selected)
{
    if (key == KEY_PREV || key == KEY_NEXT) {
        *selected = *selected == SEEDTOOL_NAV_BACK ? SEEDTOOL_NAV_SHADE : SEEDTOOL_NAV_BACK;
        return false;
    }
    if (key == KEY_SELECT && *selected == SEEDTOOL_NAV_SHADE) {
        seedtool_render_qr_cycle_shade();
        return false;
    }
    return key == KEY_SELECT;
}

/* An account key or descriptor as animated BBQr parts
 * (github.com/coinkite/BBQr), the wire format Krux reads (src/krux/bbqr.py):
 * too long for a legible single frame. Always the plain base32 "2" encoding,
 * since there is no deflate here. Returns when the reader leaves. */
static void show_account_key_qr(const char* title, const char* value)
{
    const size_t len = strlen(value);
    const size_t frame_chars = seedtool_render_qr_alphanumeric_capacity(BBQR_FRAME_MAX_VERSION);
    const size_t parts = seedtool_bbqr_part_count(len, frame_chars);
    if (!parts) {
        notice("Too long for a QR", title, "Read it as text instead");
        return;
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
            return;
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
        if (qr_key(key, &selected)) {
            return;
        }
    }
}

/* The optional passphrase, set from Derivation; a session starts without one.
 * Returns true only when it changed, so the caller re-derives the fingerprint.
 * Backing out leaves it as it was.
 *
 * Typed, read back whole, then typed again. Each step backs out to the one
 * before it with what was typed still there, and a second entry that differs
 * is retyped against the first rather than costing both. */
static bool edit_session_passphrase(char passphrase[SEEDTOOL_MAX_PASSPHRASE_LEN + 1])
{
    const char* options[] = { "No passphrase", "Enter passphrase" };
    const int selected = choose_menu("Passphrase", options, ARRAY_LEN(options));
    if (selected == NAV_BACK) {
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
    enum { STAGE_ENTER, STAGE_REVIEW, STAGE_INTRO, STAGE_CONFIRM, STAGE_MISMATCH, STAGE_DONE };
    int stage = STAGE_ENTER;
    bool ok = false;
    while (stage != STAGE_DONE) {
        switch (stage) {
        case STAGE_ENTER:
            stage = enter_passphrase_once(attempt, sizeof(attempt)) ? STAGE_REVIEW : STAGE_DONE;
            break;
        case STAGE_REVIEW: {
            /* The count says what the page cannot show: a space at either
             * end. */
            char title[32];
            (void)snprintf(title, sizeof(title), "Passphrase, %u chars", (unsigned)strlen(attempt));
            stage = page_text(title, attempt) ? STAGE_INTRO : STAGE_ENTER;
            break;
        }
        case STAGE_INTRO:
            stage = nav_acknowledge(
                        "Confirm passphrase", "Enter it a second time", "Exact match required", "Enter again", false)
                ? STAGE_CONFIRM
                : STAGE_REVIEW;
            break;
        case STAGE_CONFIRM:
            seedtool_zero(confirmation, sizeof(confirmation));
            if (!enter_passphrase_once(confirmation, sizeof(confirmation))) {
                stage = STAGE_INTRO;
            } else if (strcmp(attempt, confirmation) == 0) {
                ok = true;
                stage = STAGE_DONE;
            } else {
                stage = STAGE_MISMATCH;
            }
            break;
        default:
            /* The back arrow reads the first entry again, where a typo in it
             * would show. */
            stage = nav_acknowledge("Passphrase mismatch", "Second entry differs", "First entry kept", "Retype", false)
                ? STAGE_CONFIRM
                : STAGE_REVIEW;
            break;
        }
    }
    if (ok) {
        /* The old value goes before the new one lands, not after: this buffer
         * holds a live session secret and must never briefly hold a mix. */
        seedtool_zero(passphrase, SEEDTOOL_MAX_PASSPHRASE_LEN + 1);
        memcpy(passphrase, attempt, sizeof(attempt));
    }
    seedtool_zero(attempt, sizeof(attempt));
    seedtool_zero(confirmation, sizeof(confirmation));
    return ok;
}

/* The account key's own QR, warned about up front since a photo of it reveals
 * every address it can derive -- unlike a single address's QR below, which
 * reveals nothing more than itself. */
static void export_qr(const wallet_session_t* const session, const seedtool_key_format_t format, const char* xpub)
{
    /* Opens on the arrow: a photo of this reveals every address of the account. */
    if (!nav_acknowledge("QR export", "Account key included", "A photo reveals every address", "Show QR", true)) {
        return;
    }
    char title[24];
    (void)snprintf(
        title, sizeof(title), "BIP%u %s", (unsigned)session->type, format == SEEDTOOL_ZPUB ? "zpub" : "xpub");
    char value[ACCOUNT_KEY_LEN] = { 0 };
    (void)snprintf(value, sizeof(value), "[%s/%u'/0'/%u']%s", session->fphex, (unsigned)session->type,
        (unsigned)session->account, xpub);
    show_account_key_qr(title, value);
    seedtool_zero(value, sizeof(value));
}

/* The account as a BIP380 descriptor, for a watch-only wallet to import. Always
 * xpub, since BIP380 has no SLIP-132. The chain step is BIP389's "<0;1>", so
 * one descriptor covers receive and change. */
static void show_descriptor(const wallet_session_t* const session)
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
    if (seedtool_account_xpub(&session->root, session->type, session->account, SEEDTOOL_XPUB, xpub, sizeof(xpub))
        == SEEDTOOL_OK) {
        (void)snprintf(body, sizeof(body), "%s([%s/%u'/0'/%u']%s/<0;1>/*)",
            session->type == SEEDTOOL_BIP84 ? "wpkh" : "tr", session->fphex, (unsigned)session->type,
            (unsigned)session->account, xpub);
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
            if (qr_key(wait_key(), &selected)) {
                return;
            }
            continue;
        }
        if (!show_qr(seedtool_render_qr(title, address, fallback))) {
            notice("Too long for a QR", title, "Read it as text instead");
            return;
        }
        /* The fallback answers the same keys; the chord on the arrow goes on to
         * the text. */
        if (qr_key(wait_key(), &fallback)) {
            (void)page_text_impl(title, address, false, true);
            return;
        }
    }
}

/* The listed addresses, derived once per visit rather than on every return
 * from a QR. Addresses are public, so keeping them for the visit trades
 * privacy, not the seed. */
static char addresses[ADDRESS_SHOWN_ROWS][SEEDTOOL_MAX_ADDRESS_LEN];
static char address_labels[ADDRESS_SHOWN_ROWS][ADDRESS_LABEL_LEN];
static const char* address_items[ADDRESS_SHOWN_ROWS + 2]; /* + Go to index + Back */

/* Fills the list for one type, account and branch. Returns false, having told
 * the reader, on a derivation error. */
static bool derive_addresses(
    const wallet_session_t* const session, const seedtool_chain_t chain, const char* const prefix)
{
    /* Cleared on the way in too, so a failed derivation leaves no earlier rows. */
    seedtool_zero(address_labels, sizeof(address_labels));
    screen_text(prefix, "Deriving addresses...", NULL, NULL);
    if (seedtool_mainnet_addresses(
            &session->root, session->type, session->account, chain, 0, ADDRESS_SHOWN_ROWS, addresses)
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

/* Browses the list. Returns the chosen index with its address in
 * address_out, or NAV_BACK. `cursor` is where the list opens and where it is
 * left; Go to index moves it only within the listed rows, and derives an index
 * past them on its own. */
static int browse_addresses(const wallet_session_t* const session, const seedtool_chain_t chain,
    const char* const prefix, char address_out[SEEDTOOL_MAX_ADDRESS_LEN], size_t* cursor)
{
    for (;;) {
        const int selected = choose_menu_at(prefix, address_items, ADDRESS_SHOWN_ROWS + 1, cursor);
        if (selected == (int)ADDRESS_SHOWN_ROWS) {
            uint32_t index = 0;
            if (!enter_number("Go to index", ADDRESS_INDEX_DIGITS, &index)) {
                continue;
            }
            if (index < ADDRESS_SHOWN_ROWS) {
                *cursor = index;
                memcpy(address_out, addresses[index], SEEDTOOL_MAX_ADDRESS_LEN);
                return (int)index;
            }
            char one[1][SEEDTOOL_MAX_ADDRESS_LEN];
            if (seedtool_mainnet_addresses(&session->root, session->type, session->account, chain, index, 1, one)
                != SEEDTOOL_OK) {
                notice("Error", "Could not derive address", NULL);
                continue;
            }
            memcpy(address_out, one[0], SEEDTOOL_MAX_ADDRESS_LEN);
            return (int)index;
        }
        if (selected == NAV_BACK) {
            return NAV_BACK;
        }
        /* choose_menu_at has already left the cursor on what was taken. */
        memcpy(address_out, addresses[selected], SEEDTOOL_MAX_ADDRESS_LEN);
        return selected;
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
    if (selected == NAV_BACK) {
        return false;
    }
    *type = ADDRESS_TYPES[selected].type;
    return true;
}

/* The watch-only account key for the type and account currently set, in the
 * format asked for. */
static void show_account_key(const wallet_session_t* const session, const seedtool_key_format_t format)
{
    char xpub[SEEDTOOL_MAX_XPUB_LEN] = { 0 };
    char origin[32];
    (void)snprintf(origin, sizeof(origin), "[%s/%u'/0'/%u']", session->fphex, (unsigned)session->type,
        (unsigned)session->account);
    if (seedtool_account_xpub(&session->root, session->type, session->account, format, xpub, sizeof(xpub))
        == SEEDTOOL_OK) {
        if (page_text(origin, xpub)) {
            export_qr(session, format, xpub);
        }
    } else {
        notice("Error", "Could not derive account key", NULL);
    }
    seedtool_zero(xpub, sizeof(xpub));
}

/* xpub, zpub and the descriptor: the same key three ways, behind one row since
 * each reveals every address. SLIP-132 has no taproot prefix, so zpub is BIP84
 * only. */
static void show_extended_keys(const wallet_session_t* const session)
{
    size_t cursor = 0;
    for (;;) {
        const char* items[4];
        size_t count = 0;
        items[count++] = "xpub";
        if (session->type == SEEDTOOL_BIP84) {
            items[count++] = "zpub";
        }
        items[count++] = "Descriptor";
        const int selected = choose_menu_at("Extended public key", items, count, &cursor);
        if (selected == NAV_BACK) {
            return;
        }
        if (selected == 0) {
            show_account_key(session, SEEDTOOL_XPUB);
        } else if (session->type == SEEDTOOL_BIP84 && selected == 1) {
            show_account_key(session, SEEDTOOL_ZPUB);
        } else {
            show_descriptor(session);
        }
    }
}

/* The address list for the type and account currently set, on whichever
 * branch is asked for first. */
static void show_addresses(const wallet_session_t* const session)
{
    /* Which branch, asked before the list, as xpub/zpub is. */
    const char* const branches[] = { "Receive", "Change" };
    const int branch = choose_menu("Addresses", branches, ARRAY_LEN(branches));
    if (branch == NAV_BACK) {
        return;
    }
    const seedtool_chain_t chain = branch == 0 ? SEEDTOOL_RECEIVE : SEEDTOOL_CHANGE;
    /* The path every row shares, as the title, so a long list never loses what
     * it shows. Each address's own path is this plus its index. */
    char prefix[24];
    (void)snprintf(prefix, sizeof(prefix), "m/%u'/0'/%u'/%u", (unsigned)session->type, (unsigned)session->account,
        (unsigned)chain);
    if (!derive_addresses(session, chain, prefix)) {
        return;
    }
    /* Back to the list after each address, with the cursor where it was; only
     * backing out of the list leaves. */
    size_t cursor = 0;
    for (;;) {
        char address[SEEDTOOL_MAX_ADDRESS_LEN] = { 0 };
        const int index = browse_addresses(session, chain, prefix, address, &cursor);
        if (index == NAV_BACK) {
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
    const int layout = choose_menu("Stackbit 1248", layouts, ARRAY_LEN(layouts));
    if (layout == NAV_BACK) {
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
        if (page_key(&position, count, false) == NAV_BACK) {
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
    int answer = NAV_STAY;
    /* Outside the loop so the one exit can wipe them. */
    char lines[4][24] = { { 0 } };
    while (answer == NAV_STAY) {
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
        answer = page_key(&cursor, pages, confirmable);
    }
    seedtool_zero(numbers, sizeof(numbers));
    seedtool_zero(lines, sizeof(lines));
    return answer == NAV_CONFIRM;
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
        const int selected = choose_menu_at("Backup", items, ARRAY_LEN(items), &cursor);
        if (selected == NAV_BACK) {
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
 * everything the wallet derives. */
static void show_derivation_menu(wallet_session_t* const session)
{
    size_t cursor = 0;
    for (;;) {
        char account_item[16];
        (void)snprintf(account_item, sizeof(account_item), "Account: %u", (unsigned)session->account);
        char type_item[32];
        (void)snprintf(type_item, sizeof(type_item), "Type: %s", address_type_name(session->type));
        /* Says whether one is set, never anything about what it is. */
        const char* const passphrase_item
            = session->passphrase[0] ? "Passphrase: session only" : "Passphrase: none";
        /* Widest consequence first: the passphrase changes the seed, the type
         * the path, the account a branch of it. */
        const char* items[] = { passphrase_item, type_item, account_item };
        const int selected = choose_menu_at("Derivation", items, ARRAY_LEN(items), &cursor);
        if (selected == NAV_BACK) {
            return;
        }
        if (selected == 0) {
            /* The keyboard types only what seedtool_validate_passphrase
             * accepts, so a failure here is not a reader's mistake, and the
             * session cannot go on deriving from the old key under the new
             * passphrase. */
            if (edit_session_passphrase(session->passphrase)) {
                seedtool_require(derive_root(session));
            }
        } else if (selected == 1) {
            (void)choose_address_type(&session->type);
        } else {
            uint32_t chosen = session->account;
            if (enter_number("Account", ACCOUNT_DIGITS, &chosen)) {
                session->account = chosen;
            }
        }
    }
}

void show_wallet_data(const char* mnemonic)
{
    /* No passphrase to start. It is set from Derivation like the account and
     * type, and the fingerprint in the title shows which seed is in force. */
    wallet_session_t session = { .mnemonic = mnemonic, .type = SEEDTOOL_BIP84 };
    if (!derive_root(&session)) {
        notice("Error", "Derivation failed", NULL);
        seedtool_zero(&session, sizeof(session));
        return;
    }

    /* The fingerprint titles the menu, so a reader who knows their wallet's
     * sees at a glance which one is being derived. */
    char wallet_title[sizeof("Wallet @") + 8];
    _Static_assert(sizeof(wallet_title) >= sizeof("Wallet @") + 8,
        "the wallet title must hold its label and all eight fingerprint hex digits");
    (void)snprintf(wallet_title, sizeof(wallet_title), "Wallet @%s", session.fphex);
    enum { WALLET_BACKUP, WALLET_KEYS, WALLET_DERIVATION, WALLET_ADDRESSES, WALLET_ERASE };
    size_t cursor = 0;
    for (;;) {
        const char* menu[] = {
            [WALLET_BACKUP] = "Backup",
            [WALLET_KEYS] = "Extended public key",
            [WALLET_DERIVATION] = "Derivation",
            [WALLET_ADDRESSES] = "Addresses",
            [WALLET_ERASE] = "Erase and restart",
        };
        /* No arrow: the only exit erases the session, so it is a row. */
        const int selected = choose_nav(wallet_title, menu, ARRAY_LEN(menu), NULL, false, false, &cursor);
        /* The only way out of a wallet session is a restart: C startup zeroes
         * .bss, and scrub_free_heap zeroes the old stacks, which a reset
         * alone leaves in DRAM. Display settings go back to their defaults. */
        if (selected == WALLET_ERASE) {
            seedtool_zero(&session, sizeof(session));
            seedtool_platform_restart();
        }
        switch (selected) {
        case WALLET_BACKUP:
            show_backup_menu(mnemonic);
            break;
        case WALLET_KEYS:
            show_extended_keys(&session);
            break;
        case WALLET_DERIVATION:
            show_derivation_menu(&session);
            /* The passphrase may have changed under it, and with it the
             * fingerprint this title states. */
            (void)snprintf(wallet_title, sizeof(wallet_title), "Wallet @%s", session.fphex);
            break;
        default:
            show_addresses(&session);
            break;
        }
    }
}

/* A backup is only as good as its transcription. Quizzes the middle word of
 * every three, as Jade does (process/mnemonic.c): 4 checks for 12 words, 8 for
 * 24. The middle rather than a random word, since no screen here uses the RNG.
 * A wrong word restarts the quiz; backing out of a word steps to the previous
 * one, and out of the first leaves. Returns true once a pass matches, false on
 * giving up. */
static bool confirm_backup(const char* mnemonic, const size_t count)
{
    /* The canonical parser, which validates every word. */
    uint16_t numbers[24];
    size_t total = 0;
    seedtool_require(seedtool_mnemonic_word_numbers(mnemonic, numbers, 24, &total) == SEEDTOOL_OK);
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
            if (!enter_word(index + 1, count, NULL, typed)) {
                seedtool_zero(typed, sizeof(typed));
                if (!n) {
                    seedtool_zero(numbers, sizeof(numbers));
                    return false;
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
            return true;
        }
        if (!nav_acknowledge("Word doesn't match", "Check your backup", NULL, "Try again", false)) {
            seedtool_zero(numbers, sizeof(numbers));
            return false;
        }
    }
}

/* The words, the reminder, the quiz and the wallet it unlocks, each back
 * stepping one stage. Returns false only when the reader steps back off the
 * first. */
bool review_backup_and_show_wallet(const char* mnemonic, const size_t words)
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
                = nav_screen("Confirm backup", intro, "Have your backup ready", "Start quiz", false, NULL);
            stage = taken == NAV_CONFIRM ? STAGE_QUIZ : STAGE_WORDS;
            break;
        }
        case STAGE_QUIZ: {
            if (confirm_backup(mnemonic, words)) {
                show_wallet_data(mnemonic);
                stage = STAGE_DONE;
            } else {
                /* Backing out of the quiz's first word lands on the intro,
                 * one stage, as everywhere else. */
                stage = STAGE_INTRO;
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
