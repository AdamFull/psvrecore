/* Test programs: whole SDK-built programs (main, the SDK's start-up code and libc) that the runtime loads and runs
 * after recompilation (scripts/program_check.py). Each prints what it checks; the output must match its
 * <name>.expected exactly, and main returns the number of failed checks.
 *
 * Only what is fully determined goes to the output: computed values that do not depend on timing, and the outcome of
 * each check. */

#ifndef PSVRECORE_TEST_PROGRAM_H
#define PSVRECORE_TEST_PROGRAM_H

#include <stdio.h>

static int g_failures;

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                   \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

#endif
