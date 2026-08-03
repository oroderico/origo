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

typedef struct {
    char transcript[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1];
    uint8_t hash[SEEDTOOL_HASH_LEN];
    char mnemonic[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
    size_t words;
} seedtool_generated_t;

size_t seedtool_required_events(seedtool_source_t source, size_t words);

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

seedtool_result_t seedtool_master_fingerprint(
    const char* mnemonic, const char* passphrase, uint8_t fingerprint[4]);

/* Watch-only account key at m/type'/0'/0' in standard BIP32 serialisation. The
 * caller shows it with its derivation path; no SLIP-132 variants are produced. */
seedtool_result_t seedtool_account_xpub(
    const char* mnemonic, const char* passphrase, seedtool_address_type_t type, char* output, size_t output_len);

seedtool_result_t seedtool_mainnet_address(const char* mnemonic, const char* passphrase, seedtool_address_type_t type,
    uint32_t index, char* output, size_t output_len);

void seedtool_zero(void* ptr, size_t len);

#endif
