#include "seedtool_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <wally_address.h>
#include <wally_bip32.h>
#include <wally_bip39.h>
#include <wally_core.h>
#include <wally_crypto.h>
#include <wally_script.h>

#define HARDENED BIP32_INITIAL_HARDENED_CHILD

/* SLIP-132 zpub version bytes. libwally has no notion of SLIP-132: it only
 * serialises the four plain BIP32 versions, so a zpub is built by serialising
 * the standard way and then patching these four bytes into the output. */
static const uint8_t ZPUB_VERSION[4] = { 0x04, 0xb2, 0x47, 0x46 };

static bool valid_words(const size_t words)
{
    return words == 12 || words == 24;
}

void seedtool_zero(void* ptr, const size_t len)
{
    if (ptr && len) {
        (void)wally_bzero(ptr, len);
    }
}

size_t seedtool_required_events(const seedtool_source_t source, const size_t words)
{
    if (!valid_words(words)) {
        return 0;
    }
    switch (source) {
    case SEEDTOOL_D6:
        return words == 12 ? 50 : 99;
    case SEEDTOOL_D20:
        return words == 12 ? 30 : 60;
    case SEEDTOOL_COIN:
        return words == 12 ? 128 : 256;
    case SEEDTOOL_CARDS:
        return words == 12 ? 25 : 0;
    default:
        return 0;
    }
}

size_t seedtool_min_entropy_bits(const size_t words) { return valid_words(words) ? (words == 12 ? 128 : 256) : 0; }

static bool dice_source(const seedtool_source_t source, size_t* sides)
{
    if (source == SEEDTOOL_D6) {
        *sides = 6;
        return true;
    }
    if (source == SEEDTOOL_D20) {
        *sides = 20;
        return true;
    }
    return false;
}

/* -Sum p*log2(p) over a distribution of `total` samples spread across
 * `counts[0..buckets)`, in bits per symbol. Krux's shannon_sum. */
static double shannon_bits_per_symbol(const unsigned* counts, const size_t buckets, const size_t total)
{
    double bits = 0.0;
    for (size_t i = 0; i < buckets; ++i) {
        if (counts[i]) {
            const double probability = (double)counts[i] / (double)total;
            bits -= probability * log2(probability);
        }
    }
    return bits;
}

seedtool_result_t seedtool_dice_entropy_bits(
    const seedtool_source_t source, const uint8_t* values, const size_t values_len, int* bits_out)
{
    size_t sides = 0;
    if (!dice_source(source, &sides) || !bits_out) {
        return SEEDTOOL_EINVAL;
    }
    if (!values_len) {
        *bits_out = 0;
        return SEEDTOOL_OK;
    }
    if (!values) {
        return SEEDTOOL_EINVAL;
    }
    unsigned counts[20] = { 0 };
    for (size_t i = 0; i < values_len; ++i) {
        if (values[i] < 1 || values[i] > sides) {
            return SEEDTOOL_ERANGE;
        }
        ++counts[values[i] - 1];
    }
    *bits_out = (int)(shannon_bits_per_symbol(counts, sides, values_len) * (double)values_len);
    return SEEDTOOL_OK;
}

/* Below this many rolls, the derivative-entropy estimate below is too noisy to
 * judge: a handful of rolls can look "patterned" by chance alone. */
#define PATTERN_MIN_ROLLS 10
/* How far derivative entropy must fall below its maximum, as a percentage of
 * that maximum, to call the run patterned. Krux's PATTERN_DETECT_TOLERANCE. */
#define PATTERN_DETECT_TOLERANCE 30.0

seedtool_result_t seedtool_dice_pattern_detected(
    const seedtool_source_t source, const uint8_t* values, const size_t values_len, bool* detected_out)
{
    size_t sides = 0;
    if (!dice_source(source, &sides) || !detected_out) {
        return SEEDTOOL_EINVAL;
    }
    *detected_out = false;
    if (values_len < PATTERN_MIN_ROLLS) {
        return SEEDTOOL_OK;
    }
    if (!values) {
        return SEEDTOOL_EINVAL;
    }
    for (size_t i = 0; i < values_len; ++i) {
        if (values[i] < 1 || values[i] > sides) {
            return SEEDTOOL_ERANGE;
        }
    }
    /* Consecutive differences range over -(sides-1)..(sides-1): an operator
     * lazily counting up (or down) the die's faces leaves a run of derivatives
     * that all land on the same value or two, which collapses this
     * distribution's entropy far below its maximum. */
    const size_t derivative_range = 2 * sides - 1;
    const size_t derivatives = values_len - 1;
    unsigned counts[2 * 20 - 1] = { 0 };
    for (size_t i = 1; i < values_len; ++i) {
        const int derivative = (int)values[i] - (int)values[i - 1];
        ++counts[(size_t)(derivative + (int)sides - 1)];
    }
    const double entropy = shannon_bits_per_symbol(counts, derivative_range, derivatives);
    const double max_entropy = log2((double)derivative_range);
    const double normalized = max_entropy > 0.0 ? (max_entropy - entropy) / max_entropy * 100.0 : 0.0;
    *detected_out = normalized > PATTERN_DETECT_TOLERANCE;
    return SEEDTOOL_OK;
}

static seedtool_result_t append(char* dst, const size_t dst_len, size_t* used, const char* src)
{
    const size_t n = strlen(src);
    if (*used + n >= dst_len) {
        return SEEDTOOL_ENOSPACE;
    }
    memcpy(dst + *used, src, n);
    *used += n;
    dst[*used] = '\0';
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_transcript(const seedtool_source_t source, const uint8_t* values,
    const size_t values_len, char* output, const size_t output_len)
{
    static const char ranks[] = "A23456789TJQK";
    static const char suits[] = "CDHS";
    size_t used = 0;
    bool seen[52] = { false };
    output[0] = '\0';

    if (source == SEEDTOOL_CARDS && append(output, output_len, &used, "cards-v1:") != SEEDTOOL_OK) {
        return SEEDTOOL_ENOSPACE;
    }
    for (size_t i = 0; i < values_len; ++i) {
        char item[5];
        switch (source) {
        case SEEDTOOL_D6:
            if (values[i] < 1 || values[i] > 6) {
                return SEEDTOOL_ERANGE;
            }
            item[0] = (char)('0' + values[i]);
            item[1] = '\0';
            break;
        case SEEDTOOL_D20:
            if (values[i] < 1 || values[i] > 20) {
                return SEEDTOOL_ERANGE;
            }
            (void)snprintf(item, sizeof(item), i ? "-%u" : "%u", values[i]);
            break;
        case SEEDTOOL_COIN:
            if (values[i] > 1) {
                return SEEDTOOL_ERANGE;
            }
            item[0] = values[i] ? '1' : '0';
            item[1] = '\0';
            break;
        case SEEDTOOL_CARDS:
            if (values[i] >= 52 || seen[values[i]]) {
                return SEEDTOOL_ERANGE;
            }
            seen[values[i]] = true;
            item[0] = ranks[values[i] % 13];
            item[1] = suits[values[i] / 13];
            item[2] = '\0';
            break;
        default:
            return SEEDTOOL_EINVAL;
        }
        const seedtool_result_t ret = append(output, output_len, &used, item);
        if (ret != SEEDTOOL_OK) {
            return ret;
        }
    }
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_generate(const seedtool_source_t source, const size_t words, const uint8_t* values,
    const size_t values_len, seedtool_generated_t* output)
{
    if (!values || !output || values_len != seedtool_required_events(source, words)) {
        return SEEDTOOL_EINVAL;
    }
    memset(output, 0, sizeof(*output));
    seedtool_result_t ret = seedtool_transcript(source, values, values_len, output->transcript, sizeof(output->transcript));
    if (ret != SEEDTOOL_OK) {
        return ret;
    }
    if (wally_sha256((const uint8_t*)output->transcript, strlen(output->transcript), output->hash,
            sizeof(output->hash))
        != WALLY_OK) {
        seedtool_zero(output, sizeof(*output));
        return SEEDTOOL_ECRYPTO;
    }
    char* mnemonic = NULL;
    const size_t entropy_len = words == 12 ? 16 : 32;
    if (bip39_mnemonic_from_bytes(NULL, output->hash, entropy_len, &mnemonic) != WALLY_OK || !mnemonic) {
        seedtool_zero(output, sizeof(*output));
        return SEEDTOOL_ECRYPTO;
    }
    if (strlen(mnemonic) > SEEDTOOL_MAX_MNEMONIC_LEN) {
        wally_free_string(mnemonic);
        seedtool_zero(output, sizeof(*output));
        return SEEDTOOL_ENOSPACE;
    }
    strcpy(output->mnemonic, mnemonic);
    output->words = words;
    wally_free_string(mnemonic);
    return SEEDTOOL_OK;
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
            return SEEDTOOL_ERANGE;
        }
        size_t i = 0;
        for (; i < 2048; ++i) {
            if (strcmp(word, bip39_get_word_by_index(NULL, i)) == 0) {
                indices[(*count)++] = (uint16_t)i;
                break;
            }
        }
        if (i == 2048) {
            seedtool_zero(copy, sizeof(copy));
            return SEEDTOOL_EINVAL;
        }
    }
    seedtool_zero(copy, sizeof(copy));
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_complete_checksum(const char* prefix_mnemonic, const uint8_t* coin_bits,
    const size_t coin_bits_len, char* output, const size_t output_len)
{
    uint16_t indices[23];
    uint8_t entropy[32] = { 0 };
    size_t count = 0;
    seedtool_result_t ret = mnemonic_indices(prefix_mnemonic, indices, 23, &count);
    const size_t required_bits = count == 11 ? 7 : count == 23 ? 3 : 0;
    const size_t entropy_len = count == 11 ? 16 : count == 23 ? 32 : 0;
    if (ret != SEEDTOOL_OK || !required_bits || !coin_bits || coin_bits_len != required_bits || !output) {
        ret = SEEDTOOL_EINVAL;
        goto done;
    }

    size_t bitpos = 0;
    for (size_t i = 0; i < count; ++i) {
        for (int bit = 10; bit >= 0; --bit, ++bitpos) {
            entropy[bitpos / 8] |= ((indices[i] >> bit) & 1u) << (7 - (bitpos % 8));
        }
    }
    for (size_t i = 0; i < required_bits; ++i, ++bitpos) {
        if (coin_bits[i] > 1) {
            ret = SEEDTOOL_ERANGE;
            goto done;
        }
        entropy[bitpos / 8] |= coin_bits[i] << (7 - (bitpos % 8));
    }

    char* mnemonic = NULL;
    if (bitpos != entropy_len * 8 || bip39_mnemonic_from_bytes(NULL, entropy, entropy_len, &mnemonic) != WALLY_OK
        || !mnemonic) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (strlen(mnemonic) + 1 > output_len) {
        ret = SEEDTOOL_ENOSPACE;
    } else {
        strcpy(output, mnemonic);
        ret = SEEDTOOL_OK;
    }
    wally_free_string(mnemonic);
done:
    seedtool_zero(indices, sizeof(indices));
    seedtool_zero(entropy, sizeof(entropy));
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
        && written == (words == 12 ? 16u : 32u)) {
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

static seedtool_result_t root_from_mnemonic(
    const char* mnemonic, const char* passphrase, struct ext_key* root, uint8_t seed[64])
{
    size_t written = 0;
    if (!root || !seed || seedtool_validate_mnemonic(mnemonic, NULL) != SEEDTOOL_OK
        || seedtool_validate_passphrase(passphrase) != SEEDTOOL_OK
        || bip39_mnemonic_to_seed(mnemonic, passphrase, seed, 64, &written) != WALLY_OK || written != 64
        || bip32_key_from_seed(seed, written, BIP32_VER_MAIN_PRIVATE, 0, root) != WALLY_OK) {
        return SEEDTOOL_ECRYPTO;
    }
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_master_fingerprint(
    const char* mnemonic, const char* passphrase, uint8_t fingerprint[4])
{
    uint8_t seed[64];
    struct ext_key root;
    seedtool_result_t ret = root_from_mnemonic(mnemonic, passphrase, &root, seed);
    if (ret == SEEDTOOL_OK && bip32_key_get_fingerprint(&root, fingerprint, 4) != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
    }
    seedtool_zero(seed, sizeof(seed));
    seedtool_zero(&root, sizeof(root));
    return ret;
}

seedtool_result_t seedtool_account_xpub(const char* mnemonic, const char* passphrase,
    const seedtool_address_type_t type, const seedtool_key_format_t format, char* output, const size_t output_len)
{
    if (!output || !output_len || (type != SEEDTOOL_BIP84 && type != SEEDTOOL_BIP86)
        || (format != SEEDTOOL_XPUB && format != SEEDTOOL_ZPUB)
        || (format == SEEDTOOL_ZPUB && type != SEEDTOOL_BIP84)) {
        return SEEDTOOL_EINVAL;
    }
    uint8_t seed[64];
    struct ext_key root, account;
    unsigned char bytes[BIP32_SERIALIZED_LEN];
    char* base58 = NULL;
    const uint32_t path[] = { ((uint32_t)type) | HARDENED, 0 | HARDENED, 0 | HARDENED };
    seedtool_result_t ret = root_from_mnemonic(mnemonic, passphrase, &root, seed);
    if (ret != SEEDTOOL_OK
        || bip32_key_from_parent_path(&root, path, sizeof(path) / sizeof(path[0]), BIP32_FLAG_KEY_PUBLIC, &account)
            != WALLY_OK
        || bip32_key_serialize(&account, BIP32_FLAG_KEY_PUBLIC, bytes, sizeof(bytes)) != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (format == SEEDTOOL_ZPUB) {
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
        ret = SEEDTOOL_OK;
    }
done:
    if (base58) {
        wally_free_string(base58);
    }
    seedtool_zero(seed, sizeof(seed));
    seedtool_zero(&root, sizeof(root));
    seedtool_zero(&account, sizeof(account));
    seedtool_zero(bytes, sizeof(bytes));
    return ret;
}

/* The script and address for one index under an already-derived account node
 * (m/type'/0'/0'). Both seedtool_mainnet_address and seedtool_mainnet_addresses
 * share this: only the account derivation above it differs in cost, this part
 * — two non-hardened steps from a public key, no seed or private key involved
 * — is cheap enough to repeat per address either way. */
static seedtool_result_t address_from_account(const struct ext_key* account,
    const seedtool_address_type_t type, const uint32_t index, char* output, const size_t output_len)
{
    uint8_t script[WALLY_SCRIPTPUBKEY_P2TR_LEN];
    struct ext_key child;
    char* address = NULL;
    size_t script_len = 0;
    const uint32_t path[] = { 0, index };
    seedtool_result_t ret = SEEDTOOL_OK;
    if (bip32_key_from_parent_path(account, path, sizeof(path) / sizeof(path[0]), BIP32_FLAG_KEY_PUBLIC, &child)
        != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (type == SEEDTOOL_BIP84) {
        script[0] = OP_0;
        script[1] = 20;
        memcpy(script + 2, child.hash160, 20);
        script_len = WALLY_SCRIPTPUBKEY_P2WPKH_LEN;
    } else if (wally_scriptpubkey_p2tr_from_bytes(child.pub_key, sizeof(child.pub_key), 0, script, sizeof(script),
                   &script_len)
        != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
    }
    if (wally_addr_segwit_from_bytes(script, script_len, "bc", 0, &address) != WALLY_OK || !address) {
        ret = SEEDTOOL_ECRYPTO;
        goto done;
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

static seedtool_result_t account_from_mnemonic(const char* mnemonic, const char* passphrase,
    const seedtool_address_type_t type, struct ext_key* account)
{
    uint8_t seed[64];
    struct ext_key root;
    const uint32_t path[] = { ((uint32_t)type) | HARDENED, 0 | HARDENED, 0 | HARDENED };
    seedtool_result_t ret = root_from_mnemonic(mnemonic, passphrase, &root, seed);
    if (ret == SEEDTOOL_OK
        && bip32_key_from_parent_path(&root, path, sizeof(path) / sizeof(path[0]), BIP32_FLAG_KEY_PUBLIC, account)
            != WALLY_OK) {
        ret = SEEDTOOL_ECRYPTO;
    }
    seedtool_zero(seed, sizeof(seed));
    seedtool_zero(&root, sizeof(root));
    return ret;
}

seedtool_result_t seedtool_mainnet_address(const char* mnemonic, const char* passphrase,
    const seedtool_address_type_t type, const uint32_t index, char* output, const size_t output_len)
{
    if (!output || !output_len || index > SEEDTOOL_MAX_ADDRESS_INDEX
        || (type != SEEDTOOL_BIP84 && type != SEEDTOOL_BIP86)) {
        return SEEDTOOL_EINVAL;
    }
    struct ext_key account;
    seedtool_result_t ret = account_from_mnemonic(mnemonic, passphrase, type, &account);
    if (ret == SEEDTOOL_OK) {
        ret = address_from_account(&account, type, index, output, output_len);
    }
    seedtool_zero(&account, sizeof(account));
    return ret;
}

seedtool_result_t seedtool_mainnet_addresses(const char* mnemonic, const char* passphrase,
    const seedtool_address_type_t type, const uint32_t count, char addresses[][SEEDTOOL_MAX_ADDRESS_LEN])
{
    if (!addresses || !count || count > SEEDTOOL_MAX_ADDRESS_INDEX + 1
        || (type != SEEDTOOL_BIP84 && type != SEEDTOOL_BIP86)) {
        return SEEDTOOL_EINVAL;
    }
    struct ext_key account;
    seedtool_result_t ret = account_from_mnemonic(mnemonic, passphrase, type, &account);
    for (uint32_t i = 0; ret == SEEDTOOL_OK && i < count; ++i) {
        ret = address_from_account(&account, type, i, addresses[i], SEEDTOOL_MAX_ADDRESS_LEN);
    }
    seedtool_zero(&account, sizeof(account));
    return ret;
}
