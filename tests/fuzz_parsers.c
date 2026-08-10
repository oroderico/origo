/* Coverage-guided fuzzing of every core entry point that takes data a user can
 * shape: a mnemonic typed in during restore, a descriptor, a transcript's raw
 * values, the BBQr part arithmetic. Built with libFuzzer and run under ASan
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

    switch (selector % 8) {
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
    case 4: { /* transcript com valores arbitrarios, para cada fonte */
        char out[SEEDTOOL_MAX_TRANSCRIPT_LEN + 1];
        for (int s = 0; s <= 4; ++s) {
            (void)seedtool_transcript((seedtool_source_t)s, (const uint8_t*)str, n > 300 ? 300 : n, out, sizeof(out));
        }
        break;
    }
    case 5: { /* estimadores de entropia com valores fora de faixa */
        int bits = 0; bool pat = false;
        for (int s = 0; s <= 4; ++s) {
            (void)seedtool_dice_entropy_bits((seedtool_source_t)s, (const uint8_t*)str, n > 256 ? 256 : n, &bits);
            (void)seedtool_dice_pattern_detected((seedtool_source_t)s, (const uint8_t*)str, n > 256 ? 256 : n, &pat);
        }
        (void)seedtool_card_entropy_bits(n, &bits);
        (void)seedtool_card_pattern_detected((const uint8_t*)str, n > 60 ? 60 : n, &pat);
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
    default: break;
    }
    return 0;
}
