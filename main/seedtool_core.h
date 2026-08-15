#ifndef SEEDTOOL_CORE_H_
#define SEEDTOOL_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SEEDTOOL_HASH_LEN 32
/* Worst case across every (source, word count) seedtool_required_events
 * covers: 256 single-character coin flips for a 24-word mnemonic
 * (SEEDTOOL_COIN, 1 char/flip, no separator) - the largest transcript this
 * app ever builds. D20/24 words is the next largest at up to 203 (68 rolls,
 * "-" plus 1-2 digits each); D6, Cards and Cards-with-replacement all stay
 * well under that. A smaller bound here silently truncated seedtool_transcript
 * for exactly the sources/word-counts near this max, which seedtool_generate
 * then surfaced as "Could not generate seed" only after every event had
 * already been collected. */
#define SEEDTOOL_MAX_TRANSCRIPT_LEN 256

/* The most events any source can ask for, and the size every buffer that
 * collects a run is cut to. seedtool_required_events' largest return is a
 * coin's 256 flips for 24 words, and the collection loop is bounded only by
 * that return - `while (i < required)`, no check of its own - so raising a
 * count past this silently writes off the end of the arrays in
 * collect_entropy() and entropy_quality() while a seed is live in memory.
 * Naming it here is what lets those arrays be declared from one number
 * instead of three copies of 256, and what gives the static assertion below
 * something to stand on; the self-test walks every source and word count
 * against it, since the function's returns are not constant expressions. */
#define SEEDTOOL_MAX_EVENTS 256

/* A coin spends one transcript character per flip, so the transcript can
 * never be the smaller of the two. */
_Static_assert(SEEDTOOL_MAX_TRANSCRIPT_LEN >= SEEDTOOL_MAX_EVENTS,
    "a transcript must hold at least one character per collected event");

#define SEEDTOOL_MAX_MNEMONIC_LEN 240
#define SEEDTOOL_MAX_PASSPHRASE_LEN 100
#define SEEDTOOL_MAX_ADDRESS_LEN 96
#define SEEDTOOL_MAX_XPUB_LEN 112

/* Highest address index the viewer will derive. A calculation, not a stored
 * limit: raising it costs nothing but a bigger on-screen list. */
#define SEEDTOOL_MAX_ADDRESS_INDEX 99

/* Highest BIP44-style account (the hardened third path level, m/type'/0'/
 * account') the viewer will derive. Three digits comfortably covers every
 * account a wallet actually offering a chooser (rather than free-typing a
 * number) would ever reach - keeps the on-device entry a fixed-width keypad
 * like every other numeric entry here, with no need for the word-number
 * keyboard's variable-length reachable-digit pruning. */
#define SEEDTOOL_MAX_ACCOUNT_INDEX 999

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
    /* Same 52-card deck as SEEDTOOL_CARDS, but drawn with replacement: a
     * card is returned and the deck reshuffled before the next draw, rather
     * than set aside. A without-replacement deck tops out at log2(52!) -
     * about 225.6 bits, less than a 24-word mnemonic needs even drawing
     * every card - so 24 words uses this instead, at a genuine 52-sided-die
     * shape (see dice_source in seedtool_core.c), never offered as its own
     * top-level entropy source. */
    SEEDTOOL_CARDS_REPLACE,
} seedtool_source_t;

typedef enum {
    SEEDTOOL_BIP84 = 84,
    SEEDTOOL_BIP86 = 86,
} seedtool_address_type_t;

/* BIP44's chain level, the fourth path component: the branch an address is
 * derived under, m/type'/0'/account'/chain/index. Numbered as BIP44 itself
 * numbers it - and, exactly like seedtool_address_type_t above, the enumerator
 * *is* the path component rather than something mapped to one, so it is written
 * straight into the derivation path. */
typedef enum {
    SEEDTOOL_RECEIVE = 0,
    SEEDTOOL_CHANGE = 1,
} seedtool_chain_t;

/* Held rather than described. Because these values are written into the path
 * directly, renumbering them silently changes every address the device
 * derives and shows - the build would stay green, the vectors below would
 * still be the vectors, and the only symptom would be a reader sending coins
 * to a branch nobody meant. BIP44 fixes the numbering; this fixes it here. */
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

/* Bits of entropy a 12- or 24-word mnemonic needs (128/256, the same 16/32
 * bytes seedtool_generate hashes down to). Used to grade Shannon's entropy of
 * a dice roll run against, not to gate the hash itself. */
size_t seedtool_min_entropy_bits(size_t words);

/* Shannon's entropy, in bits, of the D6/D20/coin/with-replacement-card face
 * distribution in `values[0..values_len)` (a coin flip is a two-sided die
 * and a with-replacement card draw a 52-sided one, both 1-indexed like
 * every source here - not the 0/1 or 0..51 seedtool_transcript stores), and
 * whether consecutive rolls look patterned (an arithmetic run such as
 * 1,2,3,4,5,6,1,2,3,...) rather than random. Both are pure functions of the
 * values already entered: they are a UI quality signal only and never reach
 * seedtool_generate, which remains a function of the transcript alone.
 * EINVAL for SEEDTOOL_CARDS (without replacement), which is not a die - see
 * seedtool_card_entropy_bits/seedtool_card_pattern_detected. Adapted from
 * Krux's dice-roll entropy screen (github.com/selfcustody/krux,
 * src/krux/pages/new_mnemonic/dice_rolls.py). */
seedtool_result_t seedtool_dice_entropy_bits(
    seedtool_source_t source, const uint8_t* values, size_t values_len, int* bits_out);
seedtool_result_t seedtool_dice_pattern_detected(
    seedtool_source_t source, const uint8_t* values, size_t values_len, bool* detected_out);

/* seedtool_dice_entropy_bits is a plug-in (maximum-likelihood) Shannon-entropy
 * estimator, which is a textbook downward-biased estimate of the true entropy
 * for small per-face sample counts: with `sides` possible faces, its expected
 * shortfall in *total* reported bits is the Miller-Madow correction term
 * (sides-1)/(2 ln 2), independent of how many rolls were made. Callers that
 * grade seedtool_dice_entropy_bits' output against seedtool_min_entropy_bits
 * should add this back first, or a genuinely random run will read as "poor"
 * far more often than the roll counts in seedtool_required_events intend
 * (worst case, D20, is otherwise flagged on almost every random run). Returns
 * 0.0 for a non-dice source. */
double seedtool_dice_entropy_bias_bits(seedtool_source_t source);

/* Cards are drawn without replacement, so unlike a die's distribution their
 * information content is exact rather than statistically estimated: there
 * are exactly 52!/(52-drawn_count)! equally likely ordered draws of that
 * length, so seedtool_card_entropy_bits returns log2 of that count directly
 * - no Miller-Madow correction, since nothing here is being estimated from a
 * sample. EINVAL if drawn_count exceeds the deck. seedtool_card_pattern_detected
 * checks each draw's rank alone (ignoring suit) for the same kind of
 * arithmetic-run pattern seedtool_dice_pattern_detected looks for; both are a
 * UI quality signal only, exactly like their dice counterparts. */
seedtool_result_t seedtool_card_entropy_bits(size_t drawn_count, int* bits_out);
seedtool_result_t seedtool_card_pattern_detected(
    const uint8_t* values, size_t values_len, bool* detected_out);

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

/* Watch-only account key at m/type'/0'/account'. `format` is the standard
 * BIP32 xpub or, for BIP84 only, its SLIP-132 zpub: the same 78 bytes with
 * the four version bytes swapped, since libwally itself knows only the plain
 * BIP32 versions and has no notion of SLIP-132. `account` must not exceed
 * SEEDTOOL_MAX_ACCOUNT_INDEX. The caller shows it with its derivation path. */
seedtool_result_t seedtool_account_xpub(const char* mnemonic, const char* passphrase, seedtool_address_type_t type,
    uint32_t account, seedtool_key_format_t format, char* output, size_t output_len);

/* One mainnet address at m/type'/0'/account'/chain/index. `chain` selects the
 * receive or change branch; anything outside seedtool_chain_t is EINVAL rather
 * than a path component taken on trust. */
seedtool_result_t seedtool_mainnet_address(const char* mnemonic, const char* passphrase, seedtool_address_type_t type,
    uint32_t account, seedtool_chain_t chain, uint32_t index, char* output, size_t output_len);

/* Same addresses seedtool_mainnet_address would compute on the same branch at
 * each index 0..count-1, but the mnemonic-to-seed PBKDF2 and the account-level
 * derivation run once instead of once per address: repeating them a hundred
 * times, as browsing the address list does, is slow enough on hardware with no
 * EC acceleration to look like a hang. The chain step sits below the account
 * node, so switching branch re-runs only the cheap part of this.
 * `addresses` must hold at least `count` rows. */
seedtool_result_t seedtool_mainnet_addresses(const char* mnemonic, const char* passphrase,
    seedtool_address_type_t type, uint32_t account, seedtool_chain_t chain, uint32_t count,
    char addresses[][SEEDTOOL_MAX_ADDRESS_LEN]);

/* BIP380 output descriptor checksum (bitcoin/bips, bip-0380.mediawiki):
 * appends "#" plus 8 checksum characters to `descriptor`, writing the whole
 * result to `output`. A pure function of the descriptor text alone - no
 * mnemonic, no derivation, callable on whatever string the caller already
 * assembled from a watch-only key origin and xpub. EINVAL if `descriptor`
 * contains a character outside the checksum's own charset (a plain ASCII
 * structural set; shouldn't happen for anything this firmware itself
 * assembles, checked rather than trusted blindly). ENOSPACE if `output_len`
 * is too small for descriptor + "#" + 8 checksum characters + a NUL. */
seedtool_result_t seedtool_descriptor_checksum(const char* descriptor, char* output, size_t output_len);

void seedtool_zero(void* ptr, size_t len);

#endif
