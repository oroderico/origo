#ifndef SEEDTOOL_CORE_H_
#define SEEDTOOL_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "seedtool_wordlist.h"

#define SEEDTOOL_HASH_LEN 32
/* Longest transcript any source builds: 256 one-character coin flips for 24
 * words. D20/24 words is next at up to 203 characters. */
#define SEEDTOOL_MAX_TRANSCRIPT_LEN 256

/* Upper bound on seedtool_required_events (a coin's 256 flips for 24 words)
 * and the size of every buffer that collects a run. The collection loop trusts
 * that return, so a larger count would overflow those buffers; the tests
 * check every source and word count against this. */
#define SEEDTOOL_MAX_EVENTS 256

/* A coin spends one transcript character per flip. */
_Static_assert(SEEDTOOL_MAX_TRANSCRIPT_LEN >= SEEDTOOL_MAX_EVENTS,
    "a transcript must hold at least one character per collected event");

#define SEEDTOOL_MAX_MNEMONIC_LEN 240
#define SEEDTOOL_MAX_PASSPHRASE_LEN 100
#define SEEDTOOL_MAX_ADDRESS_LEN 96
#define SEEDTOOL_MAX_XPUB_LEN 112

/* Highest address index the viewer derives. */
#define SEEDTOOL_MAX_ADDRESS_INDEX 99

/* Highest account (m/type'/0'/account') the viewer derives: three digits,
 * so it is entered on a fixed-width keypad like every other number. */
#define SEEDTOOL_MAX_ACCOUNT_INDEX 999

typedef enum {
    SEEDTOOL_OK = 0,
    /* Bad input: arguments, a value out of range, an invalid mnemonic. */
    SEEDTOOL_EINVAL = -1,
    /* libwally failed on input it should have accepted. */
    SEEDTOOL_ECRYPTO = -3,
    SEEDTOOL_ENOSPACE = -4,
} seedtool_result_t;

typedef enum {
    SEEDTOOL_D6,
    SEEDTOOL_D20,
    SEEDTOOL_COIN,
    SEEDTOOL_CARDS,
    /* The same deck drawn with replacement, i.e. a 52-sided die. Used only
     * for 24 words: a full draw without replacement carries log2(52!) ~ 225.6
     * bits, short of 256. */
    SEEDTOOL_CARDS_REPLACE,
} seedtool_source_t;

typedef enum {
    SEEDTOOL_BIP84 = 84,
    SEEDTOOL_BIP86 = 86,
} seedtool_address_type_t;

/* BIP44 chain level (m/type'/0'/account'/chain/index). Like
 * seedtool_address_type_t, each enumerator is written into the path as is. */
typedef enum {
    SEEDTOOL_RECEIVE = 0,
    SEEDTOOL_CHANGE = 1,
} seedtool_chain_t;

/* Renumbering these would silently change every derived address. */
_Static_assert(SEEDTOOL_RECEIVE == 0 && SEEDTOOL_CHANGE == 1,
    "BIP44 numbers the receive branch 0 and the change branch 1, and these values are the path component");

/* SLIP-132 defines a zpub version prefix for native segwit (BIP84) accounts
 * and none for taproot: SEEDTOOL_BIP86 with SEEDTOOL_ZPUB is SEEDTOOL_EINVAL
 * rather than a silent fall-back to xpub. */
typedef enum {
    SEEDTOOL_XPUB,
    SEEDTOOL_ZPUB,
} seedtool_key_format_t;

typedef struct {
    char transcript[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1];
    uint8_t hash[SEEDTOOL_HASH_LEN];
    char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
    size_t words;
} seedtool_generated_t;

size_t seedtool_required_events(seedtool_source_t source, size_t words);

/* 128 or 256: the bits a 12- or 24-word mnemonic needs. Used to grade the
 * quality estimate below, never to gate generation. */
size_t seedtool_min_entropy_bits(size_t words);

/* Shannon entropy, in bits, of a die-like run, and whether consecutive values
 * look patterned (e.g. 1,2,3,4,...). Values are 1-indexed faces for every
 * source, coin and with-replacement cards included (unlike seedtool_transcript's
 * 0/1 and 0..51). A UI quality signal only: seedtool_generate never sees it.
 * EINVAL for SEEDTOOL_CARDS. Adapted from Krux
 * (src/krux/pages/new_mnemonic/dice_rolls.py). */
seedtool_result_t seedtool_dice_entropy_bits(
    seedtool_source_t source, const uint8_t* values, size_t values_len, int* bits_out);
seedtool_result_t seedtool_dice_pattern_detected(
    seedtool_source_t source, const uint8_t* values, size_t values_len, bool* detected_out);

/* Miller-Madow correction, (sides-1)/(2 ln 2) bits: the expected shortfall
 * of the plug-in estimator above. Add it back before grading against
 * seedtool_min_entropy_bits, or honest runs (D20 especially) read as poor.
 * 0.0 for a non-dice source. */
double seedtool_dice_entropy_bias_bits(seedtool_source_t source);

/* Cards without replacement: the entropy is exact, log2(52!/(52-n)!), not
 * estimated. EINVAL past 52 cards. The pattern check looks at rank alone.
 * UI quality signals only, like their dice counterparts. */
seedtool_result_t seedtool_card_entropy_bits(size_t drawn_count, int* bits_out);
seedtool_result_t seedtool_card_pattern_detected(
    const uint8_t* values, size_t values_len, bool* detected_out);

/* Canonical transcript of the first `values_len` events. A prefix of a run
 * yields a prefix of the full transcript, so it can be shown as it grows. */
seedtool_result_t seedtool_transcript(
    seedtool_source_t source, const uint8_t* values, size_t values_len, char* output, size_t output_len);

/* Values are numeric: D6=1..6, D20=1..20, coin=0/1. Cards are
 * 0..51 in canonical AC..KC, AD..KD, AH..KH, AS..KS order. */
seedtool_result_t seedtool_generate(
    seedtool_source_t source, size_t words, const uint8_t* values, size_t values_len, seedtool_generated_t* output);

/* Complete 11 or 23 known BIP39 words with respectively 7 or 3 user coin
 * flips. The coin bits are interpreted in entry order, most significant first. */
seedtool_result_t seedtool_complete_checksum(
    const char* prefix_mnemonic, const uint8_t* coin_bits, size_t coin_bits_len, char* output, size_t output_len);

/* Every word that can legally end an 11- or 23-word prefix, written into
 * `allowed`: 128 candidates for 12 words (7 free bits + 4 checksum bits), 8
 * for 24 (3 + 8). Each value of the free bits fixes its checksum, so this is
 * one SHA256 per candidate and no wordlist search. */
seedtool_result_t seedtool_final_word_candidates(const char* prefix_mnemonic, seedtool_wordset_t* allowed);

seedtool_result_t seedtool_validate_mnemonic(const char* mnemonic, size_t* words_out);
seedtool_result_t seedtool_validate_passphrase(const char* passphrase);

/* One-based wordlist positions (`abandon`=1, `zoo`=2048), as on a
 * Stackbit 1248 plate. Does not check the checksum. EINVAL for no words. */
seedtool_result_t seedtool_mnemonic_word_numbers(
    const char* mnemonic, uint16_t* numbers, size_t capacity, size_t* count_out);

/* The BIP39 entropy a valid mnemonic encodes: 16 or 32 bytes into `entropy`,
 * which must hold SEEDTOOL_HASH_LEN. This is the Compact SeedQR payload, i.e.
 * the whole wallet. */
seedtool_result_t seedtool_mnemonic_entropy(
    const char* mnemonic, uint8_t* entropy, size_t capacity, size_t* len_out);

seedtool_result_t seedtool_master_fingerprint(
    const char* mnemonic, const char* passphrase, uint8_t fingerprint[4]);

/* Watch-only account key at m/type'/0'/account', as an xpub or (BIP84 only)
 * a SLIP-132 zpub. */
seedtool_result_t seedtool_account_xpub(const char* mnemonic, const char* passphrase, seedtool_address_type_t type,
    uint32_t account, seedtool_key_format_t format, char* output, size_t output_len);

/* Mainnet addresses at m/type'/0'/account'/chain/0..count-1. PBKDF2 and the
 * hardened account derivation run once rather than per address, which would
 * be slow enough on the ESP32 to look like a hang. `chain` outside
 * seedtool_chain_t is EINVAL. `addresses` must hold `count` rows, and is
 * zeroed on failure. */
seedtool_result_t seedtool_mainnet_addresses(const char* mnemonic, const char* passphrase,
    seedtool_address_type_t type, uint32_t account, seedtool_chain_t chain, uint32_t count,
    char addresses[][SEEDTOOL_MAX_ADDRESS_LEN]);

/* Writes `descriptor` + "#" + its 8-character BIP380 checksum to `output`,
 * which must not overlap it. EINVAL for a character outside BIP380's charset,
 * ENOSPACE if it does not fit. */
seedtool_result_t seedtool_descriptor_checksum(const char* descriptor, char* output, size_t output_len);

void seedtool_zero(void* ptr, size_t len);

#endif
