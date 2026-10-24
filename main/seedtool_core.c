#include "seedtool_core.h"

#include <stdio.h>
#include <string.h>

#include <wally_address.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_core.h>
#include <wally_crypto.h>
#include <wally_script.h>

#define HARDENED BIP32_INITIAL_HARDENED_CHILD

/* SLIP-132 zpub version bytes, patched over a plain BIP32 serialisation
 * because libwally knows only the BIP32 versions. */
static const uint8_t YPUB_VERSION[4] = { 0x04, 0x9d, 0x7c, 0xb2 };
static const uint8_t ZPUB_VERSION[4] = { 0x04, 0xb2, 0x47, 0x46 };

static bool valid_words(const size_t words)
{
    return words == 12 || words == 24;
}

/* Bytes of BIP39 entropy behind a 12- or 24-word mnemonic. */
static size_t entropy_len(const size_t words) { return words == 12 ? 16 : 32; }

static bool valid_account(const seedtool_address_type_t type, const uint32_t account_index)
{
    return (type == SEEDTOOL_BIP44 || type == SEEDTOOL_BIP49 || type == SEEDTOOL_BIP84 || type == SEEDTOOL_BIP86)
        && account_index <= SEEDTOOL_MAX_ACCOUNT_INDEX;
}

void seedtool_zero(void* ptr, const size_t len)
{
    if (ptr && len) {
        (void)wally_bzero(ptr, len);
    }
}

static seedtool_result_t mnemonic_indices(const char* mnemonic, uint16_t* indices, const size_t capacity, size_t* count)
{
    if (!mnemonic || !indices || !count) {
        return SEEDTOOL_EINVAL;
    }
    char copy[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
    const size_t len = strlen(mnemonic);
    if (!len || len > SEEDTOOL_MAX_MNEMONIC_LEN) {
        return SEEDTOOL_EINVAL;
    }
    memcpy(copy, mnemonic, len + 1);
    *count = 0;
    char* save = NULL;
    for (char* word = strtok_r(copy, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
        if (*count >= capacity) {
            seedtool_zero(copy, sizeof(copy));
            return SEEDTOOL_EINVAL;
        }
        const int index = seedtool_word_index(word);
        if (index < 0) {
            seedtool_zero(copy, sizeof(copy));
            return SEEDTOOL_EINVAL;
        }
        indices[(*count)++] = (uint16_t)index;
    }
    seedtool_zero(copy, sizeof(copy));
    return SEEDTOOL_OK;
}

/* Writes the low `bits` of `value` into `bytes` at bit `pos`, most significant
 * first, as BIP39 packs word indices. */
static void put_bits(uint8_t* bytes, size_t pos, const size_t bits, const unsigned value)
{
    for (size_t b = 0; b < bits; ++b, ++pos) {
        const uint8_t mask = (uint8_t)(1u << (7 - pos % 8));
        if ((value >> (bits - 1 - b)) & 1u) {
            bytes[pos / 8] |= mask;
        } else {
            bytes[pos / 8] &= (uint8_t)~mask;
        }
    }
}

/* The last word of a 12- or 24-word mnemonic: `free_bits` of entropy followed
 * by `checksum_bits` of SHA256 over the whole entropy. */
typedef struct {
    size_t packed_bits;
    size_t free_bits;
    size_t checksum_bits;
    size_t entropy_len;
} prefix_t;

/* Packs an 11- or 23-word prefix into the leading bits of `entropy`. */
static seedtool_result_t pack_prefix(const char* prefix_mnemonic, uint8_t entropy[32], prefix_t* prefix)
{
    uint16_t indices[23];
    size_t count = 0;
    seedtool_result_t ret = mnemonic_indices(prefix_mnemonic, indices, 23, &count);
    if (ret == SEEDTOOL_OK && count != 11 && count != 23) {
        ret = SEEDTOOL_EINVAL;
    }
    if (ret == SEEDTOOL_OK) {
        prefix->packed_bits = count * 11;
        prefix->free_bits = count == 11 ? 7 : 3;
        prefix->checksum_bits = 11 - prefix->free_bits;
        prefix->entropy_len = entropy_len(count + 1);
        memset(entropy, 0, 32);
        for (size_t i = 0; i < count; ++i) {
            put_bits(entropy, i * 11, 11, indices[i]);
        }
    }
    seedtool_zero(indices, sizeof(indices));
    return ret;
}

seedtool_result_t seedtool_ticket_word(const unsigned ticket, const unsigned die, uint16_t* index_out)
{
    if (!index_out || ticket < 1 || ticket > SEEDTOOL_TICKETS || die < 1 || die > SEEDTOOL_TICKET_DIE) {
        return SEEDTOOL_EINVAL;
    }
    const unsigned number = (ticket - 1) * SEEDTOOL_TICKET_DIE + die;
    if (number > SEEDTOOL_WORDLIST_LEN) {
        return SEEDTOOL_EINVAL;
    }
    *index_out = (uint16_t)(number - 1);
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_word_ticket(const uint16_t index, unsigned* ticket_out, unsigned* die_out)
{
    if (!ticket_out || !die_out || index >= SEEDTOOL_WORDLIST_LEN) {
        return SEEDTOOL_EINVAL;
    }
    *ticket_out = (unsigned)index / SEEDTOOL_TICKET_DIE + 1;
    *die_out = (unsigned)index % SEEDTOOL_TICKET_DIE + 1;
    return SEEDTOOL_OK;
}

unsigned seedtool_die_faces(const seedtool_die_t die) { return die == SEEDTOOL_D6 ? 4 : 16; }

unsigned seedtool_die_bits(const seedtool_die_t die) { return die == SEEDTOOL_D6 ? 2 : 4; }

size_t seedtool_die_rolls_per_word(const seedtool_die_t die) { return die == SEEDTOOL_D6 ? 6 : 3; }

seedtool_result_t seedtool_die_word(
    const seedtool_die_t die, const uint8_t* rolls, const size_t count, uint16_t* index_out)
{
    if ((die != SEEDTOOL_D6 && die != SEEDTOOL_D20) || !rolls || !index_out || count != seedtool_die_rolls_per_word(die)) {
        return SEEDTOOL_EINVAL;
    }
    const unsigned faces = seedtool_die_faces(die);
    const unsigned bits = seedtool_die_bits(die);
    /* What the last roll keeps: whatever of the eleven the others left. */
    const unsigned last_bits = SEEDTOOL_WORD_BITS - bits * (unsigned)(count - 1);
    unsigned index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (rolls[i] < 1 || rolls[i] > faces) {
            return SEEDTOOL_EINVAL;
        }
        const unsigned value = rolls[i] - 1u;
        index = i + 1 < count ? (index << bits) | value : (index << last_bits) | (value >> (bits - last_bits));
    }
    *index_out = (uint16_t)index;
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_complete_checksum(const char* prefix_mnemonic, const uint8_t* coin_bits,
    const size_t coin_bits_len, char* output, const size_t output_len)
{
    uint8_t entropy[32];
    prefix_t prefix;
    char* mnemonic = NULL;
    unsigned tail = 0;
    seedtool_result_t ret = pack_prefix(prefix_mnemonic, entropy, &prefix);
    if (ret != SEEDTOOL_OK || !coin_bits || coin_bits_len != prefix.free_bits || !output) {
        ret = SEEDTOOL_EINVAL;
        goto done;
    }
    for (size_t i = 0; i < coin_bits_len; ++i) {
        if (coin_bits[i] > 1) {
            ret = SEEDTOOL_EINVAL;
            goto done;
        }
        tail = tail << 1 | coin_bits[i];
    }
    put_bits(entropy, prefix.packed_bits, prefix.free_bits, tail);
    if (bip39_mnemonic_from_bytes(NULL, entropy, prefix.entropy_len, &mnemonic) != WALLY_OK || !mnemonic) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (strlen(mnemonic) + 1 > output_len) {
        ret = SEEDTOOL_ENOSPACE;
    } else {
        strcpy(output, mnemonic);
        ret = SEEDTOOL_OK;
    }
done:
    if (mnemonic) {
        wally_free_string(mnemonic);
    }
    seedtool_zero(&tail, sizeof(tail));
    seedtool_zero(entropy, sizeof(entropy));
    return ret;
}

seedtool_result_t seedtool_final_word_candidates(const char* prefix_mnemonic, seedtool_wordset_t* allowed)
{
    uint8_t entropy[32];
    uint8_t hash[SEEDTOOL_HASH_LEN];
    prefix_t prefix;
    seedtool_result_t ret = pack_prefix(prefix_mnemonic, entropy, &prefix);
    if (ret != SEEDTOOL_OK || !allowed) {
        ret = SEEDTOOL_EINVAL;
        goto done;
    }
    seedtool_wordset_clear(allowed);
    for (unsigned tail = 0; tail < (1u << prefix.free_bits); ++tail) {
        put_bits(entropy, prefix.packed_bits, prefix.free_bits, tail);
        if (wally_sha256(entropy, prefix.entropy_len, hash, sizeof(hash)) != WALLY_OK) {
            ret = SEEDTOOL_ECRYPTO;
            goto done;
        }
        const unsigned checksum = hash[0] >> (8 - prefix.checksum_bits);
        seedtool_wordset_add(allowed, (tail << prefix.checksum_bits) | checksum);
    }
done:
    seedtool_zero(entropy, sizeof(entropy));
    seedtool_zero(hash, sizeof(hash));
    return ret;
}

seedtool_result_t seedtool_validate_mnemonic(const char* mnemonic, size_t* words_out)
{
    uint16_t indices[24];
    size_t count = 0;
    const seedtool_result_t parsed = mnemonic_indices(mnemonic, indices, 24, &count);
    seedtool_zero(indices, sizeof(indices));
    if (parsed != SEEDTOOL_OK || !valid_words(count) || bip39_mnemonic_validate(NULL, mnemonic) != WALLY_OK) {
        return SEEDTOOL_EINVAL;
    }
    if (words_out) {
        *words_out = count;
    }
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_mnemonic_word_numbers(
    const char* mnemonic, uint16_t* numbers, const size_t capacity, size_t* count_out)
{
    const seedtool_result_t ret = mnemonic_indices(mnemonic, numbers, capacity, count_out);
    if (ret != SEEDTOOL_OK) {
        return ret;
    }
    /* All spaces parses to zero words; callers divide by the count. */
    if (!*count_out) {
        return SEEDTOOL_EINVAL;
    }
    for (size_t i = 0; i < *count_out; ++i) {
        numbers[i] = (uint16_t)(numbers[i] + 1);
    }
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_mnemonic_entropy(
    const char* mnemonic, uint8_t* entropy, const size_t capacity, size_t* len_out)
{
    if (!entropy || !len_out || capacity < SEEDTOOL_HASH_LEN) {
        return SEEDTOOL_EINVAL;
    }
    size_t words = 0;
    if (seedtool_validate_mnemonic(mnemonic, &words) != SEEDTOOL_OK) {
        return SEEDTOOL_EINVAL;
    }
    size_t written = 0;
    seedtool_result_t ret = SEEDTOOL_ECRYPTO;
    if (bip39_mnemonic_to_bytes(NULL, mnemonic, entropy, capacity, &written) == WALLY_OK
        && written == entropy_len(words)) {
        *len_out = written;
        ret = SEEDTOOL_OK;
    }
    if (ret != SEEDTOOL_OK) {
        seedtool_zero(entropy, capacity);
    }
    return ret;
}

seedtool_result_t seedtool_validate_passphrase(const char* passphrase)
{
    if (!passphrase || strlen(passphrase) > SEEDTOOL_MAX_PASSPHRASE_LEN) {
        return SEEDTOOL_EINVAL;
    }
    for (const unsigned char* p = (const unsigned char*)passphrase; *p; ++p) {
        if (*p < 0x20 || *p > 0x7e) {
            return SEEDTOOL_EINVAL;
        }
    }
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_root_from_mnemonic(const char* mnemonic, const char* passphrase, seedtool_root_t* root)
{
    if (!root || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_validate_passphrase(passphrase) != SEEDTOOL_OK) {
        return SEEDTOOL_EINVAL;
    }
    uint8_t seed[BIP39_SEED_LEN_512];
    size_t written = 0;
    seedtool_result_t ret = SEEDTOOL_OK;
    if (bip39_mnemonic_to_seed(mnemonic, passphrase, seed, sizeof(seed), &written) != WALLY_OK
        || written != sizeof(seed)
        || bip32_key_from_seed(seed, written, BIP32_VER_MAIN_PRIVATE, 0, &root->key) != WALLY_OK) {
        seedtool_zero(root, sizeof(*root));
        ret = SEEDTOOL_ECRYPTO;
    }
    seedtool_zero(seed, sizeof(seed));
    return ret;
}

seedtool_result_t seedtool_master_fingerprint(const seedtool_root_t* root, uint8_t fingerprint[4])
{
    if (!root || !fingerprint) {
        return SEEDTOOL_EINVAL;
    }
    /* What bip32_key_get_fingerprint returns for a key whose hash160 was not
     * skipped, without its non-const parameter. */
    memcpy(fingerprint, root->key.hash160, 4);
    return SEEDTOOL_OK;
}

/* The watch-only account node m/type'/0'/account'. */
static seedtool_result_t account_from_root(const seedtool_root_t* root, const seedtool_address_type_t type,
    const uint32_t account_index, struct ext_key* account)
{
    const uint32_t path[] = { ((uint32_t)type) | HARDENED, 0 | HARDENED, account_index | HARDENED };
    if (!root
        || bip32_key_from_parent_path(&root->key, path, sizeof(path) / sizeof(path[0]), BIP32_FLAG_KEY_PUBLIC, account)
            != WALLY_OK) {
        return root ? SEEDTOOL_ECRYPTO : SEEDTOOL_EINVAL;
    }
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_account_xpub(const seedtool_root_t* root, const seedtool_address_type_t type,
    const uint32_t account_index, const seedtool_key_format_t format, char* output, const size_t output_len)
{
    const bool slip132_ok = format == SEEDTOOL_XPUB || (format == SEEDTOOL_YPUB && type == SEEDTOOL_BIP49)
        || (format == SEEDTOOL_ZPUB && type == SEEDTOOL_BIP84);
    if (!output || !output_len || !valid_account(type, account_index) || !slip132_ok) {
        return SEEDTOOL_EINVAL;
    }
    struct ext_key account;
    unsigned char bytes[BIP32_SERIALIZED_LEN];
    char* base58 = NULL;
    seedtool_result_t ret = account_from_root(root, type, account_index, &account);
    if (ret != SEEDTOOL_OK) {
        goto done;
    }
    if (bip32_key_serialize(&account, BIP32_FLAG_KEY_PUBLIC, bytes, sizeof(bytes)) != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (format == SEEDTOOL_YPUB) {
        memcpy(bytes, YPUB_VERSION, sizeof(YPUB_VERSION));
    } else if (format == SEEDTOOL_ZPUB) {
        memcpy(bytes, ZPUB_VERSION, sizeof(ZPUB_VERSION));
    }
    if (wally_base58_from_bytes(bytes, sizeof(bytes), BASE58_FLAG_CHECKSUM, &base58) != WALLY_OK || !base58) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (strlen(base58) + 1 > output_len) {
        ret = SEEDTOOL_ENOSPACE;
    } else {
        strcpy(output, base58);
    }
done:
    if (base58) {
        wally_free_string(base58);
    }
    seedtool_zero(&account, sizeof(account));
    seedtool_zero(bytes, sizeof(bytes));
    return ret;
}

/* The receive or change node below an account node, derived once for a run of
 * addresses rather than once per address: each public derivation is a point
 * multiplication, the cost of an address. */
static seedtool_result_t chain_from_account(
    const struct ext_key* account, const seedtool_chain_t chain, struct ext_key* chain_key)
{
    return bip32_key_from_parent(account, (uint32_t)chain, BIP32_FLAG_KEY_PUBLIC, chain_key) == WALLY_OK
        ? SEEDTOOL_OK
        : SEEDTOOL_ECRYPTO;
}

/* Address at `index` below a chain node: one public, non-hardened derivation,
 * cheap enough to repeat per address. */
static seedtool_result_t address_from_chain(const struct ext_key* chain_key, const seedtool_address_type_t type,
    const uint32_t index, char* output, const size_t output_len)
{
    uint8_t script[WALLY_SCRIPTPUBKEY_P2TR_LEN];
    struct ext_key child;
    char* address = NULL;
    size_t script_len = 0;
    seedtool_result_t ret = SEEDTOOL_OK;
    if (bip32_key_from_parent(chain_key, index, BIP32_FLAG_KEY_PUBLIC, &child) != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (type == SEEDTOOL_BIP44 || type == SEEDTOOL_BIP49) {
        /* Base58 with a version byte: P2PKH over the key's hash160, or P2SH
         * over the hash160 of the P2WPKH script it wraps (BIP49). */
        uint8_t payload[1 + HASH160_LEN];
        if (type == SEEDTOOL_BIP44) {
            payload[0] = WALLY_ADDRESS_VERSION_P2PKH_MAINNET;
            memcpy(payload + 1, child.hash160, HASH160_LEN);
        } else {
            script[0] = OP_0;
            script[1] = HASH160_LEN;
            memcpy(script + 2, child.hash160, HASH160_LEN);
            payload[0] = WALLY_ADDRESS_VERSION_P2SH_MAINNET;
            if (wally_hash160(script, WALLY_SCRIPTPUBKEY_P2WPKH_LEN, payload + 1, HASH160_LEN) != WALLY_OK) {
                ret = SEEDTOOL_ECRYPTO;
                goto done;
            }
        }
        if (wally_base58_from_bytes(payload, sizeof(payload), BASE58_FLAG_CHECKSUM, &address) != WALLY_OK
            || !address) {
            ret = SEEDTOOL_ECRYPTO;
            goto done;
        }
    } else {
        if (type == SEEDTOOL_BIP84) {
            script[0] = OP_0;
            script[1] = HASH160_LEN;
            memcpy(script + 2, child.hash160, HASH160_LEN);
            script_len = WALLY_SCRIPTPUBKEY_P2WPKH_LEN;
        } else if (wally_scriptpubkey_p2tr_from_bytes(
                       child.pub_key, sizeof(child.pub_key), 0, script, sizeof(script), &script_len)
            != WALLY_OK) {
            ret = SEEDTOOL_ECRYPTO;
            goto done;
        }
        if (wally_addr_segwit_from_bytes(script, script_len, "bc", 0, &address) != WALLY_OK || !address) {
            ret = SEEDTOOL_ECRYPTO;
            goto done;
        }
    }
    if (strlen(address) + 1 > output_len) {
        ret = SEEDTOOL_ENOSPACE;
    } else {
        strcpy(output, address);
    }
done:
    if (address) {
        wally_free_string(address);
    }
    seedtool_zero(script, sizeof(script));
    seedtool_zero(&child, sizeof(child));
    return ret;
}

seedtool_result_t seedtool_mainnet_addresses(const seedtool_root_t* root, const seedtool_address_type_t type,
    const uint32_t account_index, const seedtool_chain_t chain, const uint32_t first, const uint32_t count,
    char addresses[][SEEDTOOL_MAX_ADDRESS_LEN])
{
    if (!addresses || !count || first > SEEDTOOL_MAX_ADDRESS_INDEX || count > SEEDTOOL_MAX_ADDRESS_INDEX + 1 - first
        || !valid_account(type, account_index)
        || (chain != SEEDTOOL_RECEIVE && chain != SEEDTOOL_CHANGE)) {
        return SEEDTOOL_EINVAL;
    }
    struct ext_key account, chain_key;
    seedtool_result_t ret = account_from_root(root, type, account_index, &account);
    if (ret == SEEDTOOL_OK) {
        ret = chain_from_account(&account, chain, &chain_key);
    }
    for (uint32_t i = 0; ret == SEEDTOOL_OK && i < count; ++i) {
        ret = address_from_chain(&chain_key, type, first + i, addresses[i], SEEDTOOL_MAX_ADDRESS_LEN);
    }
    if (ret != SEEDTOOL_OK) {
        seedtool_zero(addresses, count * SEEDTOOL_MAX_ADDRESS_LEN);
    }
    seedtool_zero(&account, sizeof(account));
    seedtool_zero(&chain_key, sizeof(chain_key));
    return ret;
}

seedtool_result_t seedtool_find_address(const seedtool_root_t* root, const seedtool_address_type_t type,
    const uint32_t account_index, const char* ending, const uint32_t first, const uint32_t count,
    seedtool_chain_t* chain_out, uint32_t* index_out, bool* found_out)
{
    const size_t ending_len = ending ? strnlen(ending, SEEDTOOL_MAX_ADDRESS_LEN) : 0;
    if (!ending_len || ending_len >= SEEDTOOL_MAX_ADDRESS_LEN || !chain_out || !index_out || !found_out || !count
        || first > SEEDTOOL_MAX_ADDRESS_INDEX || count > SEEDTOOL_MAX_ADDRESS_INDEX + 1 - first
        || !valid_account(type, account_index)) {
        return SEEDTOOL_EINVAL;
    }
    *found_out = false;
    static const seedtool_chain_t chains[] = { SEEDTOOL_RECEIVE, SEEDTOOL_CHANGE };
    struct ext_key account, chain_keys[2];
    char address[SEEDTOOL_MAX_ADDRESS_LEN];
    seedtool_result_t ret = account_from_root(root, type, account_index, &account);
    for (size_t c = 0; ret == SEEDTOOL_OK && c < 2; ++c) {
        ret = chain_from_account(&account, chains[c], &chain_keys[c]);
    }
    /* Index by index, receive then change, so the addresses a wallet has
     * actually used, low on both branches, are reached first. */
    for (uint32_t i = 0; ret == SEEDTOOL_OK && !*found_out && i < count; ++i) {
        for (size_t c = 0; ret == SEEDTOOL_OK && c < 2; ++c) {
            ret = address_from_chain(&chain_keys[c], type, first + i, address, sizeof(address));
            const size_t len = ret == SEEDTOOL_OK ? strlen(address) : 0;
            if (len >= ending_len && !strcmp(address + len - ending_len, ending)) {
                *chain_out = chains[c];
                *index_out = first + i;
                *found_out = true;
                break;
            }
        }
    }
    seedtool_zero(&account, sizeof(account));
    seedtool_zero(chain_keys, sizeof(chain_keys));
    seedtool_zero(address, sizeof(address));
    return ret;
}

/* Charsets and generator transcribed verbatim from bip-0380.mediawiki. */
static const char DESCRIPTOR_INPUT_CHARSET[]
    = "0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";
static const char DESCRIPTOR_CHECKSUM_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static int descriptor_charset_index(const char c)
{
    for (int i = 0; DESCRIPTOR_INPUT_CHARSET[i]; ++i) {
        if (DESCRIPTOR_INPUT_CHARSET[i] == c) {
            return i;
        }
    }
    return -1;
}

/* One step of BIP380's polymod (bech32's construction, other generator). */
static uint64_t descriptor_polymod_step(uint64_t chk, const uint32_t value)
{
    static const uint64_t generator[5]
        = { 0xf5dee51989ULL, 0xa9fdca3312ULL, 0x1bab10e32dULL, 0x3706b1677aULL, 0x644d626ffdULL };
    const uint64_t top = chk >> 35;
    chk = ((chk & 0x7ffffffffULL) << 5) ^ value;
    for (int i = 0; i < 5; ++i) {
        if ((top >> i) & 1) {
            chk ^= generator[i];
        }
    }
    return chk;
}

seedtool_result_t seedtool_descriptor_checksum(const char* descriptor, char* output, const size_t output_len)
{
    if (!descriptor || !output) {
        return SEEDTOOL_EINVAL;
    }
    const size_t len = strlen(descriptor);
    if (len + 1 + 8 + 1 > output_len) {
        return SEEDTOOL_ENOSPACE;
    }
    /* BIP380's descsum_expand: low 5 bits per character, plus the high bits
     * of each group of three folded into one extra symbol. */
    uint64_t chk = 1;
    unsigned cls = 0;
    unsigned clscount = 0;
    for (size_t i = 0; i < len; ++i) {
        const int v = descriptor_charset_index(descriptor[i]);
        if (v < 0) {
            return SEEDTOOL_EINVAL;
        }
        chk = descriptor_polymod_step(chk, (uint32_t)v & 31);
        cls = cls * 3 + ((uint32_t)v >> 5);
        if (++clscount == 3) {
            chk = descriptor_polymod_step(chk, cls);
            cls = 0;
            clscount = 0;
        }
    }
    if (clscount > 0) {
        chk = descriptor_polymod_step(chk, cls);
    }
    for (int i = 0; i < 8; ++i) {
        chk = descriptor_polymod_step(chk, 0);
    }
    chk ^= 1;
    memcpy(output, descriptor, len);
    output[len] = '#';
    for (size_t i = 0; i < 8; ++i) {
        output[len + 1 + i] = DESCRIPTOR_CHECKSUM_CHARSET[(chk >> (5 * (7 - i))) & 31];
    }
    output[len + 9] = '\0';
    return SEEDTOOL_OK;
}
