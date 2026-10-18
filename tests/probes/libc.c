/* The SDK's C library, recompiled: string and memory functions, conversions, character classes, and qsort/bsearch
 * calling back into the probe (indirect calls between modules). The host side calls the host's C library: every
 * function used is one whose result the C standard fixes for these inputs (no equal keys for qsort, no locale). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "probe.h"

/* (the pointers themselves volatile: neither compiler may evaluate the library calls on them at compile time) */
static const char* volatile kWords[] = {
    "probe", "", "a", "zebra", "Recompiled", "static", "0x7fffffff", "-12345", "  42abc", "1e10", "PS Vita", "abcabcabd",
};
#define WORD_COUNT (sizeof kWords / sizeof kWords[0])

static int compare_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static uint32_t sign(int v) { return v < 0 ? 0xFFFFFFFFu : v > 0 ? 1u : 0u; }

uint32_t probe_main(uint32_t* out) {
    ProbeOut o = {out, 0};
    for (uint32_t i = 0; i < WORD_COUNT; ++i) {
        const char* s = (const char*)kWords[i];
        put(&o, (uint32_t)strlen(s));
        const char* c = strchr(s, 'a');
        put(&o, c ? (uint32_t)(c - s) : 0xFFFFFFFFu);
        c = strrchr(s, 'b');
        put(&o, c ? (uint32_t)(c - s) : 0xFFFFFFFFu);
        c = strstr(s, "bc");
        put(&o, c ? (uint32_t)(c - s) : 0xFFFFFFFFu);
        put(&o, (uint32_t)strtol(s, 0, 0));
        put(&o, (uint32_t)strtoul(s, 0, 10));
        put(&o, (uint32_t)atoi(s));
        uint32_t classes = 0;
        for (uint32_t k = 0; s[k]; ++k)
            classes = classes * 31 + (isdigit((unsigned char)s[k]) ? 1u : 0u) + (isalpha((unsigned char)s[k]) ? 2u : 0u) +
                      (isspace((unsigned char)s[k]) ? 4u : 0u) + (uint32_t)toupper((unsigned char)s[k]) * 8u;
        put(&o, classes);
        for (uint32_t j = 0; j < WORD_COUNT; ++j) {
            const char* t = (const char*)kWords[j];
            put(&o, sign(strcmp(s, t)));
            put(&o, sign(strncmp(s, t, 3)));
            put(&o, sign(memcmp(s, t, strlen(s) < strlen(t) ? strlen(s) : strlen(t))));
        }
    }
    {
        uint8_t buffer[300];
        memset(buffer, 0x5A, sizeof buffer);
        for (uint32_t i = 0; i < 64; ++i) buffer[i] = (uint8_t)(i * 7);
        memcpy(buffer + 100, buffer, 64);
        memmove(buffer + 10, buffer, 100);   /* overlapping, forward */
        memmove(buffer + 150, buffer + 160, 90);  /* overlapping, backward */
        memset(buffer + 250, 0, 33);
        uint32_t h = 0;
        for (uint32_t i = 0; i < sizeof buffer; ++i) h = h * 131 + buffer[i];
        put(&o, h);
        put(&o, buffer[7] | (buffer[123] << 8) | (buffer[200] << 16) | ((uint32_t)buffer[299] << 24));
    }
    {
        uint32_t values[97];
        uint32_t x = 12345;
        for (uint32_t i = 0; i < 97; ++i) {
            x = x * 1103515245u + 12345u;
            values[i] = (x >> 8) * 97 + i;  /* (distinct: no two compare equal) */
        }
        qsort(values, 97, sizeof values[0], compare_u32);
        for (uint32_t i = 0; i < 97; ++i) put(&o, values[i]);
        for (uint32_t i = 0; i < 97; i += 7) {
            const uint32_t key = values[i];
            const uint32_t* found = (const uint32_t*)bsearch(&key, values, 97, sizeof values[0], compare_u32);
            put(&o, found ? (uint32_t)(found - values) : 0xFFFFFFFFu);
        }
        const uint32_t missing = 1;
        put(&o, bsearch(&missing, values, 97, sizeof values[0], compare_u32) ? 1u : 0u);
    }
    put(&o, (uint32_t)abs(-17));
    put(&o, (uint32_t)labs(-170000L));
    {
        const div_t d = div(-17, 5);
        put(&o, (uint32_t)d.quot);
        put(&o, (uint32_t)d.rem);
    }
    return o.n;
}
