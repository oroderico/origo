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

size_t seedtool_required_events(const seedtool_source_t source, const size_t words)
{
    if (!valid_words(words)) {
        return 0;
    }
    switch (source) {
    case SEEDTOOL_D6:
        /* Margin over the 50/99-roll minimum so an honest run's estimate
         * does not read short of 128/256 bits (see
         * honest_runs_report_at_least_the_minimum). */
        return words == 12 ? 60 : 120;
    case SEEDTOOL_D20:
        /* Well past the 30/60-roll minimum: the biased estimator has the
         * least sampling margin on D20. */
        return words == 12 ? 36 : 68;
    case SEEDTOOL_COIN:
        return words == 12 ? 128 : 256;
    case SEEDTOOL_CARDS:
        return words == 12 ? 25 : 0;
    case SEEDTOOL_CARDS_REPLACE:
        /* Chosen by simulation, like D6 (see
         * dice_entropy_false_positive_rate_is_bounded). 24 words only. */
        return words == 24 ? 50 : 0;
    default:
        return 0;
    }
}

size_t seedtool_min_entropy_bits(const size_t words) { return valid_words(words) ? (words == 12 ? 128 : 256) : 0; }

/* Largest side count dice_source() returns; sizes the buffers below. */
#define DICE_SOURCE_MAX_SIDES 52

/* Every source that is an N-sided die. SEEDTOOL_CARDS is not one. */
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
    if (source == SEEDTOOL_COIN) {
        *sides = 2;
        return true;
    }
    if (source == SEEDTOOL_CARDS_REPLACE) {
        *sides = DICE_SOURCE_MAX_SIDES;
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
    unsigned counts[DICE_SOURCE_MAX_SIDES] = { 0 };
    for (size_t i = 0; i < values_len; ++i) {
        if (values[i] < 1 || values[i] > sides) {
            return SEEDTOOL_EINVAL;
        }
        ++counts[values[i] - 1];
    }
    *bits_out = (int)(shannon_bits_per_symbol(counts, sides, values_len) * (double)values_len);
    return SEEDTOOL_OK;
}

double seedtool_dice_entropy_bias_bits(const seedtool_source_t source)
{
    size_t sides = 0;
    if (!dice_source(source, &sides)) {
        return 0.0;
    }
    return ((double)sides - 1.0) / (2.0 * log(2.0));
}

/* Fewer rolls than this are too noisy to call patterned. */
#define PATTERN_MIN_ROLLS 10
/* How far derivative entropy must fall below its maximum, as a percentage of
 * that maximum, to call the run patterned. Krux's PATTERN_DETECT_TOLERANCE. */
#define PATTERN_DETECT_TOLERANCE 30.0

/* Entropy of consecutive differences, which collapses when someone counts
 * through the faces instead of rolling. `sides` <= DICE_SOURCE_MAX_SIDES. */
static bool derivative_pattern_detected(const uint8_t* values_1indexed, const size_t values_len, const size_t sides)
{
    const size_t derivative_range = 2 * sides - 1;
    const size_t derivatives = values_len - 1;
    unsigned counts[2 * DICE_SOURCE_MAX_SIDES - 1] = { 0 };
    for (size_t i = 1; i < values_len; ++i) {
        const int derivative = (int)values_1indexed[i] - (int)values_1indexed[i - 1];
        ++counts[(size_t)(derivative + (int)sides - 1)];
    }
    const double entropy = shannon_bits_per_symbol(counts, derivative_range, derivatives);
    const double max_entropy = log2((double)derivative_range);
    const double normalized = max_entropy > 0.0 ? (max_entropy - entropy) / max_entropy * 100.0 : 0.0;
    return normalized > PATTERN_DETECT_TOLERANCE;
}

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
            return SEEDTOOL_EINVAL;
        }
    }
    *detected_out = derivative_pattern_detected(values, values_len, sides);
    return SEEDTOOL_OK;
}

seedtool_result_t seedtool_card_entropy_bits(const size_t drawn_count, int* bits_out)
{
    if (!bits_out || drawn_count > 52) {
        return SEEDTOOL_EINVAL;
    }
    double bits = 0.0;
    for (size_t i = 0; i < drawn_count; ++i) {
        bits += log2((double)(52 - i));
    }
    *bits_out = (int)bits;
    return SEEDTOOL_OK;
}

/* Rank alone (1..13): counting through a deck walks ranks, not suits. */
seedtool_result_t seedtool_card_pattern_detected(
    const uint8_t* values, const size_t values_len, bool* detected_out)
{
    if (!detected_out) {
        return SEEDTOOL_EINVAL;
    }
    *detected_out = false;
    if (values_len < PATTERN_MIN_ROLLS) {
        return SEEDTOOL_OK;
    }
    if (!values || values_len > 52) {
        return SEEDTOOL_EINVAL;
    }
    uint8_t ranks[52];
    for (size_t i = 0; i < values_len; ++i) {
        if (values[i] >= 52) {
            return SEEDTOOL_EINVAL;
        }
        ranks[i] = (uint8_t)(values[i] % 13 + 1);
    }
    *detected_out = derivative_pattern_detected(ranks, values_len, 13);
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
    if (!output || !output_len || (values_len && !values)) {
        return SEEDTOOL_EINVAL;
    }
    output[0] = '\0';

    const bool is_cards = source == SEEDTOOL_CARDS || source == SEEDTOOL_CARDS_REPLACE;
    if (is_cards && append(output, output_len, &used, SEEDTOOL_CARDS_TRANSCRIPT_PREFIX) != SEEDTOOL_OK) {
        return SEEDTOOL_ENOSPACE;
    }
    for (size_t i = 0; i < values_len; ++i) {
        char item[5];
        switch (source) {
        case SEEDTOOL_D6:
            if (values[i] < 1 || values[i] > 6) {
                return SEEDTOOL_EINVAL;
            }
            item[0] = (char)('0' + values[i]);
            item[1] = '\0';
            break;
        case SEEDTOOL_D20:
            if (values[i] < 1 || values[i] > 20) {
                return SEEDTOOL_EINVAL;
            }
            (void)snprintf(item, sizeof(item), i ? "-%u" : "%u", values[i]);
            break;
        case SEEDTOOL_COIN:
            if (values[i] > 1) {
                return SEEDTOOL_EINVAL;
            }
            item[0] = values[i] ? '1' : '0';
            item[1] = '\0';
            break;
        case SEEDTOOL_CARDS:
            if (values[i] >= 52 || seen[values[i]]) {
                return SEEDTOOL_EINVAL;
            }
            seen[values[i]] = true;
            item[0] = ranks[values[i] % 13];
            item[1] = suits[values[i] / 13];
            item[2] = '\0';
            break;
        case SEEDTOOL_CARDS_REPLACE:
            /* With replacement, so repeats are valid. */
            if (values[i] >= 52) {
                return SEEDTOOL_EINVAL;
            }
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
    /* A zero count is an unsupported source or word count, not an empty run. */
    const size_t required = seedtool_required_events(source, words);
    if (!values || !output || !required || values_len != required) {
        return SEEDTOOL_EINVAL;
    }
    memset(output, 0, sizeof(*output));
    seedtool_result_t ret = seedtool_transcript(source, values, values_len, output->transcript, sizeof(output->transcript));
    if (ret != SEEDTOOL_OK) {
        seedtool_zero(output, sizeof(*output));
        return ret;
    }
    if (wally_sha256((const uint8_t*)output->transcript, strlen(output->transcript), output->hash,
            sizeof(output->hash))
        != WALLY_OK) {
        seedtool_zero(output, sizeof(*output));
        return SEEDTOOL_ECRYPTO;
    }
    char* mnemonic = NULL;
    if (bip39_mnemonic_from_bytes(NULL, output->hash, entropy_len(words), &mnemonic) != WALLY_OK || !mnemonic) {
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

/* Address at chain/index below an account node: public, non-hardened
 * derivation only, cheap enough to repeat per address. */
static seedtool_result_t address_from_account(const struct ext_key* account, const seedtool_address_type_t type,
    const seedtool_chain_t chain, const uint32_t index, char* output, const size_t output_len)
{
    uint8_t script[WALLY_SCRIPTPUBKEY_P2TR_LEN];
    struct ext_key child;
    char* address = NULL;
    size_t script_len = 0;
    const uint32_t path[] = { (uint32_t)chain, index };
    seedtool_result_t ret = SEEDTOOL_OK;
    if (bip32_key_from_parent_path(account, path, sizeof(path) / sizeof(path[0]), BIP32_FLAG_KEY_PUBLIC, &child)
        != WALLY_OK) {
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
    struct ext_key account;
    seedtool_result_t ret = account_from_root(root, type, account_index, &account);
    for (uint32_t i = 0; ret == SEEDTOOL_OK && i < count; ++i) {
        ret = address_from_account(&account, type, chain, first + i, addresses[i], SEEDTOOL_MAX_ADDRESS_LEN);
    }
    if (ret != SEEDTOOL_OK) {
        seedtool_zero(addresses, count * SEEDTOOL_MAX_ADDRESS_LEN);
    }
    seedtool_zero(&account, sizeof(account));
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
