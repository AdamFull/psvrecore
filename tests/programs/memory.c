/* Memory: libc's heap (on memory blocks it takes from the kernel), memory blocks directly, and libc functions
 * that depend on the runtime (time, formatted output, sorting). */

#include <kernel.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "program.h"

/* libc's heap is 256 KiB unless the program asks for more. */
unsigned int sceLibcHeapSize = 32 * 1024 * 1024;

static void test_heap(void) {
    static void* blocks[500];
    int i, ok = 1;
    for (i = 0; i < 500; ++i) {
        size_t size = 16 + (size_t)(i * 37 % 4096);
        blocks[i] = malloc(size);
        if (!blocks[i]) {
            ok = 0;
            break;
        }
        memset(blocks[i], i & 0xFF, size);
    }
    for (i = 0; ok && i < 500; ++i) {
        const unsigned char* p = (const unsigned char*)blocks[i];
        if (p[0] != (i & 0xFF)) ok = 0;
    }
    for (i = 0; i < 500; i += 2) free(blocks[i]);
    for (i = 0; i < 500; i += 2) blocks[i] = calloc(1, 64);
    for (i = 0; ok && i < 500; i += 2)
        if (!blocks[i] || ((unsigned char*)blocks[i])[63] != 0) ok = 0;
    for (i = 0; i < 500; ++i) free(blocks[i]);
    printf("heap: %s\n", ok ? "ok" : "corrupt");
    {
        char* big = (char*)malloc(8 * 1024 * 1024);
        CHECK(big != NULL);
        if (big) {
            big[0] = 1;
            big[8 * 1024 * 1024 - 1] = 2;
            big = (char*)realloc(big, 16 * 1024 * 1024);
            CHECK(big != NULL && big[0] == 1 && big[8 * 1024 * 1024 - 1] == 2);
            free(big);
        }
    }
}

static void test_memblock(void) {
    void* base = NULL;
    SceUID uid = sceKernelAllocMemBlock("test block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 1024 * 1024, NULL);
    CHECK(uid > 0);
    CHECK(sceKernelGetMemBlockBase(uid, &base) == 0);
    CHECK(base != NULL && ((unsigned)base & 0xFFF) == 0);
    if (base) {
        memset(base, 0x5A, 1024 * 1024);
        CHECK(((unsigned char*)base)[1024 * 1024 - 1] == 0x5A);
        CHECK(sceKernelFindMemBlockByAddr((char*)base + 100, 1) == uid);
    }
    CHECK(sceKernelAllocMemBlock("odd", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 1000, NULL) < 0);
    CHECK(sceKernelFreeMemBlock(uid) == 0);
    CHECK(sceKernelGetMemBlockBase(uid, &base) < 0);
    printf("memory block: done\n");
}

static int compare(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

static void test_libc(void) {
    int values[10] = {5, 3, 9, 1, 7, 2, 8, 0, 6, 4};
    char text[64];
    int i;
    time_t now = time(NULL);
    clock_t c0 = clock();
    qsort(values, 10, sizeof values[0], compare);
    for (i = 0; i < 10; ++i) CHECK(values[i] == i);
    snprintf(text, sizeof text, "%.2f %e %5.1f", 3.14159, 1234.5, -2.26);
    printf("formatted: %s\n", text);
    printf("strtod: %.3f\n", strtod("2.5e3", NULL));
    CHECK(now > 1600000000); /* (after 2020) */
    sceKernelDelayThread(10000);
    CHECK(clock() - c0 >= 10000); /* (microseconds) */
    {
        struct tm* utc = gmtime(&now);
        CHECK(utc != NULL && utc->tm_year >= 120 && utc->tm_mon < 12);
    }
}

int main(void) {
    test_heap();
    test_memblock();
    test_libc();
    printf("memory: %d failed\n", g_failures);
    return g_failures;
}
