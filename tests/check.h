/* The tests' one assertion and runner. A failed CHECK names its file, line
 * and expression; the runner names the test it failed in. */
#ifndef ORIGO_TEST_CHECK_H_
#define ORIGO_TEST_CHECK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition);                   \
            return false;                                                                                    \
        }                                                                                                    \
    } while (0)

typedef struct {
    const char* name;
    bool (*run)(void);
} test_t;

#define TEST(fn) { #fn, fn }

static inline int run_tests(const char* suite, const test_t* tests, const size_t count)
{
    size_t failed = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!tests[i].run()) {
            fprintf(stderr, "%s: %s FAILED\n", suite, tests[i].name);
            ++failed;
        }
    }
    printf("%s: %zu of %zu passed\n", suite, count - failed, count);
    return failed ? 1 : 0;
}

#endif
