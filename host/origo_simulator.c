#include "seedtool_app.h"
#include "seedtool_core.h"
#include "seedtool_render.h"
#include "seedtool_wordlist.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wally_core.h>

/* Published BIP84/BIP86 vectors for the all-zero entropy mnemonic. */
static const char mnemonic[]
    = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
static const char expected_address[] = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
static const char expected_xpub84[]
    = "xpub6CatWdiZiodmUeTDp8LT5or8nmbKNcuyvz7WyksVFkKB4RHwCD3XyuvPEbvqAQY3rAPshWcMLoP2fMFMKHPJ4ZeZXYVUhLv1VMrjPC7PW6V";
static const char expected_xpub86[]
    = "xpub6BgBgsespWvERF3LHQu6CnqdvfEvtMcQjYrcRzx53QJjSxarj2afYWcLteoGVky7D3UKDP9QyrLprQ3VCECoY49yfdDEHGCtMMj92pReUsQ";

/* The word-entry keyboard is only usable if every reachable letter really does
 * lead to a word, and if narrowing always terminates in a listable candidate
 * set. Both are checked exhaustively rather than by example. */
static bool wordlist_completion_is_sound(void)
{
    uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
    bool enabled[SEEDTOOL_LETTERS];

    if (seedtool_words_with_prefix("", 0, candidates, SEEDTOOL_MAX_WORD_CHOICES) != SEEDTOOL_WORDLIST_LEN
        || seedtool_next_letters("", 0, enabled) == 0) {
        return false;
    }
    for (size_t index = 0; index < SEEDTOOL_WORDLIST_LEN; ++index) {
        const char* const word = seedtool_word(index);
        const size_t length = strlen(word);
        if (!length || length > SEEDTOOL_MAX_WORD_LEN) {
            return false;
        }
        for (size_t prefix_len = 0; prefix_len < length; ++prefix_len) {
            /* Every prefix of a real word must match it, and the next letter of
             * that word must be offered by the keyboard. */
            if (!seedtool_words_with_prefix(word, prefix_len, candidates, SEEDTOOL_MAX_WORD_CHOICES)
                || !seedtool_next_letters(word, prefix_len, enabled) || !enabled[word[prefix_len] - 'a']) {
                return false;
            }
        }
        /* Typing four letters always narrows to a set the carousel can show. */
        const size_t stem = length < 4 ? length : 4;
        if (seedtool_words_with_prefix(word, stem, candidates, SEEDTOOL_MAX_WORD_CHOICES)
            > SEEDTOOL_MAX_WORD_CHOICES) {
            return false;
        }
    }
    return true;
}

/* Paging splits by pixel width, so the chunks must still reassemble byte for
 * byte: a dropped character in a displayed xpub or address would be transcribed
 * as fact. */
static bool paging_is_lossless(const char* text)
{
    char rebuilt[512] = { 0 };
    const size_t total = strlen(text);
    size_t offset = 0, used = 0;
    while (offset < total) {
        const size_t fit = seedtool_render_fit(text + offset, 48);
        if (!fit || used + fit >= sizeof(rebuilt)) {
            return false;
        }
        memcpy(rebuilt + used, text + offset, fit);
        used += fit;
        offset += fit;
    }
    rebuilt[used] = '\0';
    return strcmp(rebuilt, text) == 0;
}

static int self_test(void)
{
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    char xpub[SEEDTOOL_MAX_XPUB_LEN];
    if (wally_init(0) != WALLY_OK || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_mainnet_address(mnemonic, "", SEEDTOOL_BIP84, 0, address, sizeof(address)) != SEEDTOOL_OK
        || strcmp(address, expected_address) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP84, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub84) != 0
        || seedtool_account_xpub(mnemonic, "", SEEDTOOL_BIP86, xpub, sizeof(xpub)) != SEEDTOOL_OK
        || strcmp(xpub, expected_xpub86) != 0) {
        fputs("Origo host self-test failed\n", stderr);
        return 1;
    }
    if (!wordlist_completion_is_sound()) {
        fputs("Origo wordlist completion self-test failed\n", stderr);
        return 1;
    }
    if (!paging_is_lossless(expected_xpub84) || !paging_is_lossless(expected_xpub86)
        || !paging_is_lossless(expected_address) || !paging_is_lossless(mnemonic)
        || !paging_is_lossless("cards-v1:ACKS7D2H")) {
        fputs("Origo paging self-test failed\n", stderr);
        return 1;
    }
    seedtool_render_screen("ORIGO", "HOST SELF-TEST", address, "OK");
    if (!seedtool_render_qr(address)) {
        fputs("Origo QR self-test failed\n", stderr);
        return 1;
    }
    /* Exercise both keyboards so a layout that overflows a row is caught here. */
    const bool letters[SEEDTOOL_LETTERS + 1] = { true };
    seedtool_render_keyboard("Word 1 of 12", "aba", "abcdefghij\nklmnopqrs\ntuvwxyz\b", letters, 0);
    seedtool_render_keyboard("BIP39 passphrase", "", "abcdefghij\nklmnopqrst\nuvwxyz \b\t\r", NULL, 29);
    seedtool_zero(address, sizeof(address));
    seedtool_zero(xpub, sizeof(xpub));
    wally_cleanup(0);
    puts("Origo host self-test OK");
    return 0;
}

int main(const int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0) {
        return self_test();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--self-test]\n", argv[0]);
        return 2;
    }
    seedtool_run();
}
