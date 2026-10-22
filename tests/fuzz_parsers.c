/* Coverage-guided fuzzing of every core entry point that takes data a user can
 * shape: a mnemonic typed in during restore, a descriptor, a word's rolls or
 * ticket, the BBQr part arithmetic. Built with libFuzzer and run under ASan
 * and UBSan, so the bar is not "fails gracefully" but "never reads or writes
 * outside a buffer" - which is the property the firmware's fixed-size arrays
 * rest on and which reading the code can only argue for.
 *
 * Not part of the CI gate: coverage-guided fuzzing has no natural stopping
 * point and a time-boxed run is a weak gate. Run it by hand after touching a
 * parser, from the repository root, with a toolchain whose sanitizer runtime
 * is installed (clang on Fedora; GCC there ships the link stubs without
 * libasan):
 *
 *   cmake -S host -B build-host -DCMAKE_BUILD_TYPE=RelWithDebInfo
 *   cmake --build build-host --target wally_host
 *   clang -fsanitize=fuzzer,address,undefined -g -std=gnu11 \
 *     -Imain -Icomponents/libwally-core/upstream/include \
 *     main/seedtool_core.c main/seedtool_bbqr.c main/seedtool_wordlist.c \
 *     tests/fuzz_parsers.c build-host/libwally_host.a -lm -o /tmp/origo-fuzz
 *   /tmp/origo-fuzz -max_total_time=300
 *
 * libwally is linked uninstrumented on purpose: it is vendored and trusted,
 * and secp256k1's 64-bit inline assembly will not compile under the
 * instrumentation at all.
 *
 * Last clean run: 3.1 million executions, no findings. */
#include "seedtool_core.h"
#include "seedtool_bbqr.h"
#include "seedtool_wordlist.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Toda funcao do core que aceita dado moldavel pelo usuario. O alvo nao e
 * "nao crashar de forma elegante" - e nao ler nem escrever fora dos buffers,
 * que e o que o ASAN observa por baixo. */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 2) return 0;
    const uint8_t selector = data[0];
    ++data; --size;

    char str[512];
    const size_t n = size < sizeof(str) - 1 ? size : sizeof(str) - 1;
    memcpy(str, data, n);
    str[n] = '\0';

    switch (selector % 10) {
    case 0: {
        size_t words = 0;
        (void)seedtool_validate_mnemonic(str, &words);
        break;
    }
    case 1: {
        uint8_t entropy[SEEDTOOL_HASH_LEN];
        size_t len = 0;
        (void)seedtool_mnemonic_entropy(str, entropy, sizeof(entropy), &len);
        break;
    }
    case 2: {
        uint16_t numbers[24];
        size_t count = 0;
        (void)seedtool_mnemonic_word_numbers(str, numbers, 24, &count);
        break;
    }
    case 3: {
        char out[256];
        (void)seedtool_descriptor_checksum(str, out, sizeof(out));
        break;
    }
    case 4: { /* bilhete e dado do SeedPicker, e a volta, com valores arbitrarios */
        if (n < 4) break;
        uint16_t index = 0;
        unsigned ticket = 0, die = 0;
        (void)seedtool_ticket_word((uint8_t)str[0] | (unsigned)(uint8_t)str[1] << 8, (uint8_t)str[2], &index);
        (void)seedtool_word_ticket((uint16_t)((uint8_t)str[2] | (uint8_t)str[3] << 8), &ticket, &die);
        break;
    }
    case 5: { /* rolagens de dado com faces e contagem arbitrarias, dado invalido incluso */
        uint16_t index = 0;
        for (int d = 0; d <= 2; ++d) {
            (void)seedtool_die_word((seedtool_die_t)d, (const uint8_t*)str, n > 8 ? 8 : n, &index);
        }
        break;
    }
    case 6: { /* BBQr: contagem e indice vindos do proprio input */
        if (n < 3) break;
        char out[512];
        const size_t pc = (str[0] % 8) + 1, pi = str[1] % 8;
        (void)seedtool_bbqr_part((const uint8_t*)str + 2, n - 2, 'Z', 'U', pi, pc, out, sizeof(out));
        (void)seedtool_bbqr_part_count(n, (str[2] % 200) + 10);
        char b32[1024];
        (void)seedtool_bbqr_base32_encode((const uint8_t*)str, n, b32, sizeof(b32));
        break;
    }
    case 7: { /* completar checksum com bits de moeda arbitrarios */
        char out[SEEDTOOL_MAX_MNEMONIC_LEN + 1];
        uint8_t coin[8];
        for (size_t i = 0; i < sizeof(coin); ++i) coin[i] = (uint8_t)(n ? str[i % n] : 0);
        for (size_t len = 0; len <= 8; ++len) {
            (void)seedtool_complete_checksum(str, coin, len, out, sizeof(out));
        }
        break;
    }
    case 8: { /* wordlist: comprimento vindo do proprio input, nao do que str
               * de fato contem - mesma forma do pc/pi do case 6 */
        uint16_t candidates[SEEDTOOL_MAX_WORD_CHOICES];
        bool letters[SEEDTOOL_LETTERS];
        bool digits[SEEDTOOL_DIGITS];
        const size_t prefix_len = n ? (size_t)(uint8_t)str[0] % (sizeof(str) - 1) : 0;
        (void)seedtool_words_with_prefix_in(NULL, str, prefix_len, candidates, SEEDTOOL_MAX_WORD_CHOICES);
        (void)seedtool_next_letters_in(NULL, str, prefix_len, letters);
        (void)seedtool_next_digits_in(NULL, str, prefix_len, digits);
        (void)seedtool_word_number_in(NULL, str, prefix_len);
        break;
    }
    case 9: { /* passphrase: texto livre digitado pelo usuario */
        (void)seedtool_validate_passphrase(str);
        break;
    }
    default: break;
    }
    return 0;
}
