#ifndef SEEDTOOL_CORE_H_
#define SEEDTOOL_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_HASH_LEN 32
#define SEEDTOOL_MAX_TRANSCRIPT_LEN 180
#define SEEDTOOL_MAX_MNEMONIC_LEN 240
#define SEEDTOOL_MAX_PASSPHRASE_LEN 100
#define SEEDTOOL_MAX_ADDRESS_LEN 96
#define SEEDTOOL_MAX_XPUB_LEN 112

/* Highest address index the viewer will derive. A calculation, not a stored
 * limit: raising it costs nothing but a bigger on-screen list. */
#define SEEDTOOL_MAX_ADDRESS_INDEX 99

typedef enum {
    SEEDTOOL_OK = 0,
    SEEDTOOL_EINVAL = -1,
    SEEDTOOL_ERANGE = -2,
    SEEDTOOL_ECRYPTO = -3,
    SEEDTOOL_ENOSPACE = -4,
} seedtool_result_t;

typedef enum {
    SEEDTOOL_D6,
    SEEDTOOL_D20,
    SEEDTOOL_COIN,
    SEEDTOOL_CARDS,
} seedtool_source_t;

typedef enum {
    SEEDTOOL_BIP84 = 84,
    SEEDTOOL_BIP86 = 86,
} seedtool_address_type_t;

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

/* Bits of entropy a 12- or 24-word mnemonic needs (128/256, the same 16/32
 * bytes seedtool_generate hashes down to). Used to grade Shannon's entropy of
 * a dice roll run against, not to gate the hash itself. */
size_t seedtool_min_entropy_bits(size_t words);

/* Shannon's entropy, in bits, of the D6/D20 face distribution in
 * `values[0..values_len)`, and whether consecutive rolls look patterned (an
 * arithmetic run such as 1,2,3,4,5,6,1,2,3,...) rather than random. Both are
 * pure functions of the values already entered: they are a UI quality signal
 * only and never reach seedtool_generate, which remains a function of the
 * transcript alone. EINVAL for any source other than SEEDTOOL_D6/SEEDTOOL_D20.
 * Adapted from Krux's dice-roll entropy screen
 * (github.com/selfcustody/krux, src/krux/pages/new_mnemonic/dice_rolls.py). */
seedtool_result_t seedtool_dice_entropy_bits(
    seedtool_source_t source, const uint8_t* values, size_t values_len, int* bits_out);
seedtool_result_t seedtool_dice_pattern_detected(
    seedtool_source_t source, const uint8_t* values, size_t values_len, bool* detected_out);

/* The canonical transcript of the first `values_len` events. Callable with a
 * prefix of a run: what it writes is byte for byte the start of what the whole
 * run will write, which is what lets the screen show a running history that the
 * reader can compare against paper as it grows. */
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

seedtool_result_t seedtool_validate_mnemonic(const char* mnemonic, size_t* words_out);
seedtool_result_t seedtool_validate_passphrase(const char* passphrase);

/* One-based word numbers for a Stackbit-1248-style backup: the position of
 * each word in the printed BIP39 English wordlist, `abandon`=1, `zoo`=2048 —
 * the same convention enter_word_number() already restores a plate by. Does
 * not itself check the checksum; pair with seedtool_validate_mnemonic first
 * when that matters. `numbers` must hold at least `capacity` entries; 24 is
 * enough for any mnemonic this firmware generates or restores. */
seedtool_result_t seedtool_mnemonic_word_numbers(
    const char* mnemonic, uint16_t* numbers, size_t capacity, size_t* count_out);

/* The raw entropy a mnemonic encodes: the exact BIP39 inverse of
 * seedtool_generate's bip39_mnemonic_from_bytes call, byte for byte what
 * seedtool_generate's own hash field held (its leading 16 or 32 bytes),
 * whether the mnemonic just came from that generation or was typed in during
 * restore. `entropy` must hold at least SEEDTOOL_HASH_LEN bytes; `len_out` is
 * 16 for a 12-word mnemonic, 32 for a 24-word one. This is the exact payload
 * of a Compact SeedQR: total, irreversible compromise of the wallet if it is
 * ever photographed. Rejects a bad checksum exactly as
 * seedtool_validate_mnemonic does. */
seedtool_result_t seedtool_mnemonic_entropy(
    const char* mnemonic, uint8_t* entropy, size_t capacity, size_t* len_out);

seedtool_result_t seedtool_master_fingerprint(
    const char* mnemonic, const char* passphrase, uint8_t fingerprint[4]);

/* Watch-only account key at m/type'/0'/0'. `format` is the standard BIP32 xpub
 * or, for BIP84 only, its SLIP-132 zpub: the same 78 bytes with the four
 * version bytes swapped, since libwally itself knows only the plain BIP32
 * versions and has no notion of SLIP-132. The caller shows it with its
 * derivation path. */
seedtool_result_t seedtool_account_xpub(const char* mnemonic, const char* passphrase, seedtool_address_type_t type,
    seedtool_key_format_t format, char* output, size_t output_len);

seedtool_result_t seedtool_mainnet_address(const char* mnemonic, const char* passphrase, seedtool_address_type_t type,
    uint32_t index, char* output, size_t output_len);

void seedtool_zero(void* ptr, size_t len);

#endif
