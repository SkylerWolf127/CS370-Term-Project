#ifndef PLANTSENSE_TEST_H
#define PLANTSENSE_TEST_H

/* Tiny test harness: CHECK keeps going after a failure, main returns 1. */

#include <stdio.h>
#include <string.h>

static int t_checks;
static int t_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        t_checks++;                                                        \
        if (!(cond)) {                                                     \
            t_failures++;                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                   \
                    __FILE__, __LINE__, #cond);                            \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        long long a_ = (long long)(a), b_ = (long long)(b);                \
        t_checks++;                                                        \
        if (a_ != b_) {                                                    \
            t_failures++;                                                  \
            fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s): got %lld, want %lld\n", \
                    __FILE__, __LINE__, #a, #b, a_, b_);                   \
        }                                                                  \
    } while (0)

#define CHECK_STR(a, b)                                                    \
    do {                                                                   \
        const char *a_ = (a), *b_ = (b);                                   \
        t_checks++;                                                        \
        if (strcmp(a_, b_) != 0) {                                         \
            t_failures++;                                                  \
            fprintf(stderr, "%s:%d: CHECK_STR failed: \"%s\" != \"%s\"\n", \
                    __FILE__, __LINE__, a_, b_);                           \
        }                                                                  \
    } while (0)

static inline int test_report(const char *name)
{
    printf("%s: %d checks, %d failures\n", name, t_checks, t_failures);
    return t_failures ? 1 : 0;
}

#endif
