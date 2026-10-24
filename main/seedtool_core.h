#ifndef SEEDTOOL_CORE_H_
#define SEEDTOOL_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wally_bip32.h>

#include "seedtool_wordlist.h"

#define SEEDTOOL_HASH_LEN 32

#define SEEDTOOL_MAX_MNEMONIC_LEN 240
#define SEEDTOOL_MAX_PASSPHRASE_LEN 100
#define SEEDTOOL_MAX_ADDRESS_LEN 96
#define SEEDTOOL_MAX_XPUB_LEN 112

/* Highest address index the viewer derives: four digits on the keypad, past
 * the hundred or so a long-used wallet reaches. */
#define SEEDTOOL_MAX_ADDRESS_INDEX 9999

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

/* Script types, each the BIP number its accounts use as the path's purpose:
 * legacy P2PKH, nested segwit P2SH-P2WPKH, native segwit, taproot. */
typedef enum {
    SEEDTOOL_BIP44 = 44,
    SEEDTOOL_BIP49 = 49,
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

/* SLIP-132 defines a ypub version prefix for nested segwit (BIP49) accounts, a
 * zpub one for native segwit (BIP84), and none for legacy or taproot: any other
 * pairing is SEEDTOOL_EINVAL rather than a silent fall-back to xpub. */
typedef enum {
    SEEDTOOL_XPUB,
    SEEDTOOL_YPUB,
    SEEDTOOL_ZPUB,
} seedtool_key_format_t;

/* Complete 11 or 23 known BIP39 words with respectively 7 or 3 user coin
 * flips. The coin bits are interpreted in entry order, most significant first. */
seedtool_result_t seedtool_complete_checksum(
    const char* prefix_mnemonic, const uint8_t* coin_bits, size_t coin_bits_len, char* output, size_t output_len);

/* Every word that can legally end an 11- or 23-word prefix, written into
 * `allowed`: 128 candidates for 12 words (7 free bits + 4 checksum bits), 8
 * for 24 (3 + 8). Each value of the free bits fixes its checksum, so this is
 * one SHA256 per candidate and no wordlist search. */
seedtool_result_t seedtool_final_word_candidates(const char* prefix_mnemonic, seedtool_wordset_t* allowed);

/* SeedPicker's tickets (seedpicker.net): a ticket drawn from T1..T342 and put
 * back, then a six-sided die, name word number (ticket - 1) * 6 + die. The 2052
 * pairs cover the 2048 words once each; the four past 2048 (T342 with 3 to 6)
 * are drawn again from the ticket, which keeps every word equally likely. */
#define SEEDTOOL_TICKETS 342
#define SEEDTOOL_TICKET_DIE 6
_Static_assert(SEEDTOOL_TICKETS * SEEDTOOL_TICKET_DIE >= SEEDTOOL_WORDLIST_LEN
        && (SEEDTOOL_TICKETS - 1) * SEEDTOOL_TICKET_DIE < SEEDTOOL_WORDLIST_LEN,
    "the tickets and die must cover the wordlist with only the last ticket's tail left over");

/* The zero-based word index a ticket and die name, or SEEDTOOL_EINVAL when
 * either is out of range or the pair is one of the four past 2048. */
seedtool_result_t seedtool_ticket_word(unsigned ticket, unsigned die, uint16_t* index_out);

/* The other way: the one ticket and die that name a word, which is what the
 * device shows to check a draw against the seed form. SEEDTOOL_EINVAL for an
 * index past the wordlist. */
seedtool_result_t seedtool_word_ticket(uint16_t index, unsigned* ticket_out, unsigned* die_out);

/* A die read as bits, one word at a time. Only as many faces count as are a
 * power of two - 1 to 4 on a D6, 1 to 16 on a D20 - and the rest are rolled
 * again, so every counted face is equally likely: face f is the bits of f - 1,
 * most significant first, from a printed table. Eleven bits do not divide by 2
 * or 4, so a word's last roll keeps only its first bits: a D6 word is five
 * rolls of 2 bits and one of 1 (1 or 2 is 0, 3 or 4 is 1), a D20 word two of 4
 * bits and one of 3. */
typedef enum {
    SEEDTOOL_D6,
    SEEDTOOL_D20,
} seedtool_die_t;

/* The faces that count (4 or 16), the bits each roll gives (2 or 4; a
 * word's last roll keeps fewer) and the rolls a word takes (6 or 3). */
unsigned seedtool_die_faces(seedtool_die_t die);
unsigned seedtool_die_bits(seedtool_die_t die);
size_t seedtool_die_rolls_per_word(seedtool_die_t die);
#define SEEDTOOL_DIE_MAX_ROLLS 6

/* The zero-based word index a word's rolls name, or SEEDTOOL_EINVAL for a
 * wrong roll count or a face that does not count. */
seedtool_result_t seedtool_die_word(seedtool_die_t die, const uint8_t* rolls, size_t count, uint16_t* index_out);

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

/* The BIP32 master key of a mnemonic and passphrase. PBKDF2 is the slow step
 * of every derivation, so it runs once here and everything below derives from
 * the result. This is the private key: zero it with seedtool_zero. */
typedef struct {
    struct ext_key key;
} seedtool_root_t;

seedtool_result_t seedtool_root_from_mnemonic(const char* mnemonic, const char* passphrase, seedtool_root_t* root);

seedtool_result_t seedtool_master_fingerprint(const seedtool_root_t* root, uint8_t fingerprint[4]);

/* Watch-only account key at m/type'/0'/account', as an xpub or its SLIP-132
 * form: ypub for BIP49, zpub for BIP84. */
seedtool_result_t seedtool_account_xpub(const seedtool_root_t* root, seedtool_address_type_t type, uint32_t account,
    seedtool_key_format_t format, char* output, size_t output_len);

/* Mainnet addresses at m/type'/0'/account'/chain/first..first+count-1. The
 * hardened account derivation and the branch's run once rather than per
 * address. `chain` outside seedtool_chain_t, or an index past
 * SEEDTOOL_MAX_ADDRESS_INDEX, is EINVAL. `addresses` must hold `count` rows, and is zeroed on failure. */
seedtool_result_t seedtool_mainnet_addresses(const seedtool_root_t* root, seedtool_address_type_t type,
    uint32_t account, seedtool_chain_t chain, uint32_t first, uint32_t count,
    char addresses[][SEEDTOOL_MAX_ADDRESS_LEN]);

/* Looks for the address ending in `ending` among indices first..first+count-1
 * of both branches of m/type'/0'/account', index by index, receive before
 * change. The match is exact and case-sensitive on the address's last
 * characters. OK with `*found_out` false when none ends that way; with it true,
 * the first that does is at `*chain_out`/`*index_out`. Searching a long range
 * a window at a time lets the caller show progress and stop between windows. */
seedtool_result_t seedtool_find_address(const seedtool_root_t* root, seedtool_address_type_t type, uint32_t account,
    const char* ending, uint32_t first, uint32_t count, seedtool_chain_t* chain_out, uint32_t* index_out,
    bool* found_out);

/* Writes `descriptor` + "#" + its 8-character BIP380 checksum to `output`,
 * which must not overlap it. EINVAL for a character outside BIP380's charset,
 * ENOSPACE if it does not fit. */
seedtool_result_t seedtool_descriptor_checksum(const char* descriptor, char* output, size_t output_len);

void seedtool_zero(void* ptr, size_t len);

#endif
