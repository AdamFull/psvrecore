/* Threads and synchronisation through the kernel's own API, with libc on top. */

#include <kernel.h>
#include <string.h>

#include "program.h"

/* ---- a counter shared by four threads, under a lightweight mutex */

static SceKernelLwMutexWork g_lw;
static volatile int g_counter;

static int counter_thread(SceSize args, void* argp) {
    int i;
    int n = *(int*)argp;
    for (i = 0; i < n; ++i) {
        sceKernelLockLwMutex(&g_lw, 1, NULL);
        g_counter = g_counter + 1;
        sceKernelUnlockLwMutex(&g_lw, 1);
    }
    (void)args;
    return n;
}

static void test_lw_mutex_counter(void) {
    SceUID threads[4];
    int i, n = 20000, total = 0;
    CHECK(sceKernelCreateLwMutex(&g_lw, "counter", 0, 0, NULL) == 0);
    for (i = 0; i < 4; ++i) {
        threads[i] = sceKernelCreateThread("counter", counter_thread, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
        CHECK(threads[i] > 0);
        CHECK(sceKernelStartThread(threads[i], sizeof n, &n) == 0);
    }
    for (i = 0; i < 4; ++i) {
        int status = -1;
        CHECK(sceKernelWaitThreadEnd(threads[i], &status, NULL) == 0);
        total += status;
        sceKernelDeleteThread(threads[i]);
    }
    CHECK(sceKernelDeleteLwMutex(&g_lw) == 0);
    printf("lw mutex counter: %d of %d\n", g_counter, total);
}

/* ---- producer and consumer through a ring of 8 slots and two semaphores */

#define RING 8
static int g_ring[RING];
static SceUID g_free_slots, g_used_slots;

static int producer(SceSize args, void* argp) {
    int i;
    for (i = 1; i <= 1000; ++i) {
        sceKernelWaitSema(g_free_slots, 1, NULL);
        g_ring[i % RING] = i;
        sceKernelSignalSema(g_used_slots, 1);
    }
    (void)args;
    (void)argp;
    return 0;
}

static int consumer(SceSize args, void* argp) {
    int i, sum = 0;
    for (i = 1; i <= 1000; ++i) {
        sceKernelWaitSema(g_used_slots, 1, NULL);
        sum += g_ring[i % RING];
        sceKernelSignalSema(g_free_slots, 1);
    }
    (void)args;
    (void)argp;
    return sum;
}

static void test_semaphores(void) {
    SceUID p, c;
    int status = 0;
    g_free_slots = sceKernelCreateSema("free", 0, RING, RING, NULL);
    g_used_slots = sceKernelCreateSema("used", 0, 0, RING, NULL);
    CHECK(g_free_slots > 0 && g_used_slots > 0);
    p = sceKernelCreateThread("producer", producer, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    c = sceKernelCreateThread("consumer", consumer, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(c, 0, NULL);
    sceKernelStartThread(p, 0, NULL);
    CHECK(sceKernelWaitThreadEnd(p, NULL, NULL) == 0);
    CHECK(sceKernelWaitThreadEnd(c, &status, NULL) == 0);
    printf("semaphore ring: sum %d\n", status);
    {
        SceUInt32 timeout = 10000;
        CHECK(sceKernelWaitSema(g_used_slots, 1, &timeout) == SCE_KERNEL_ERROR_WAIT_TIMEOUT);
        CHECK(timeout == 0);
    }
    CHECK(sceKernelSignalSema(g_free_slots, 1) == SCE_KERNEL_ERROR_SEMA_OVF);
    sceKernelDeleteSema(g_free_slots);
    sceKernelDeleteSema(g_used_slots);
    sceKernelDeleteThread(p);
    sceKernelDeleteThread(c);
}

/* ---- three workers report through an event flag; the main thread waits for all of them */

static SceUID g_flag;

static int flag_worker(SceSize args, void* argp) {
    int bit = *(int*)argp;
    sceKernelDelayThread(1000 * (3 - bit));
    sceKernelSetEventFlag(g_flag, 1u << bit);
    (void)args;
    return 0;
}

static void test_event_flag(void) {
    SceUID w[3];
    int i;
    SceUInt32 result = 0;
    g_flag = sceKernelCreateEventFlag("flag", SCE_KERNEL_EVF_ATTR_MULTI, 0, NULL);
    CHECK(g_flag > 0);
    for (i = 0; i < 3; ++i) {
        w[i] = sceKernelCreateThread("flag worker", flag_worker, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
        sceKernelStartThread(w[i], sizeof i, &i);
    }
    CHECK(sceKernelWaitEventFlag(g_flag, 0x7, SCE_KERNEL_EVF_WAITMODE_AND | SCE_KERNEL_EVF_WAITMODE_CLEAR_ALL, &result, NULL) == 0);
    printf("event flag: all three bits 0x%x\n", (unsigned)result);
    CHECK(sceKernelPollEventFlag(g_flag, 0x7, SCE_KERNEL_EVF_WAITMODE_OR, &result) == SCE_KERNEL_ERROR_EVF_COND);
    for (i = 0; i < 3; ++i) {
        sceKernelWaitThreadEnd(w[i], NULL, NULL);
        sceKernelDeleteThread(w[i]);
    }
    sceKernelDeleteEventFlag(g_flag);
}

/* ---- on one CPU, ready threads run by priority; equal priorities in the order they became ready */

static char g_order[16];
static int g_order_n;

static int order_thread(SceSize args, void* argp) {
    g_order[g_order_n++] = *(char*)argp;
    (void)args;
    return 0;
}

static void test_priority_order(void) {
    static const char names[4] = {'c', 'a', 'd', 'b'};
    static const int priorities[4] = {120, 100, 120, 110};
    SceUID t[4];
    int i;
    /* The main thread on CPU 0 at the highest priority while it starts the others there, then the lowest. */
    CHECK(sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF, SCE_KERNEL_CPU_MASK_USER_0) == 0);
    CHECK(sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, 64) == 0);
    for (i = 0; i < 4; ++i) {
        t[i] = sceKernelCreateThread("order", order_thread, priorities[i], 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(t[i], 1, &names[i]);
    }
    CHECK(g_order_n == 0); /* (none has run: the main thread has the CPU) */
    CHECK(sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, 191) == 0);
    /* (the others ran when the main thread's priority dropped) */
    g_order[g_order_n] = 0;
    printf("priority order: %s\n", g_order);
    for (i = 0; i < 4; ++i) {
        sceKernelWaitThreadEnd(t[i], NULL, NULL);
        sceKernelDeleteThread(t[i]);
    }
    sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, SCE_KERNEL_DEFAULT_PRIORITY_USER);
    sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF, 0);
}

/* ---- a mutex and condition variable handing items over */

static SceUID g_mutex, g_cond;
static int g_items[4], g_count, g_done;

static int taker(SceSize args, void* argp) {
    int sum = 0, taken = 0;
    sceKernelLockMutex(g_mutex, 1, NULL);
    while (taken < 100) {
        while (g_count == 0) sceKernelWaitCond(g_cond, NULL);
        sum += g_items[--g_count];
        ++taken;
        sceKernelSignalCondAll(g_cond);
    }
    sceKernelUnlockMutex(g_mutex, 1);
    (void)args;
    (void)argp;
    return sum;
}

static void test_cond(void) {
    SceUID t;
    int i, status = 0;
    g_mutex = sceKernelCreateMutex("m", 0, 0, NULL);
    g_cond = sceKernelCreateCond("c", 0, g_mutex, NULL);
    CHECK(g_mutex > 0 && g_cond > 0);
    t = sceKernelCreateThread("taker", taker, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
    for (i = 1; i <= 100; ++i) {
        sceKernelLockMutex(g_mutex, 1, NULL);
        while (g_count == 4) sceKernelWaitCond(g_cond, NULL);
        g_items[g_count++] = i;
        sceKernelSignalCond(g_cond);
        sceKernelUnlockMutex(g_mutex, 1);
    }
    CHECK(sceKernelWaitThreadEnd(t, &status, NULL) == 0);
    printf("condition variable: sum %d\n", status);
    g_done = 1;
    sceKernelDeleteThread(t);
    sceKernelDeleteCond(g_cond);
    sceKernelDeleteMutex(g_mutex);
}

/* ---- exclusive loads and stores (atomic operations) from three threads at once */

static volatile int g_atomic;

static int atomic_thread(SceSize args, void* argp) {
    int i;
    for (i = 0; i < 100000; ++i) __sync_fetch_and_add(&g_atomic, 1);
    (void)args;
    (void)argp;
    return 0;
}

static void test_atomics(void) {
    SceUID t[3];
    int i;
    for (i = 0; i < 3; ++i) {
        t[i] = sceKernelCreateThread("atomic", atomic_thread, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
        sceKernelStartThread(t[i], 0, NULL);
    }
    for (i = 0; i < 3; ++i) {
        sceKernelWaitThreadEnd(t[i], NULL, NULL);
        sceKernelDeleteThread(t[i]);
    }
    printf("atomic adds: %d\n", g_atomic);
}

/* ---- ending a thread from inside a call; delays; thread information */

static int deep(int n) {
    if (n == 0) sceKernelExitThread(77);
    return deep(n - 1) + 1;
}

static int exiting_thread(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    return deep(10);
}

static void test_exit_and_delay(void) {
    SceUID t = sceKernelCreateThread("exiting", exiting_thread, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    int status = 0;
    SceUInt64 before, after;
    SceKernelThreadInfo info;
    sceKernelStartThread(t, 0, NULL);
    CHECK(sceKernelWaitThreadEnd(t, &status, NULL) == 0);
    printf("exit from a nested call: status %d\n", status);
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    CHECK(sceKernelGetThreadInfo(t, &info) == 0);
    CHECK(strcmp(info.name, "exiting") == 0);
    CHECK(info.status == SCE_KERNEL_THREAD_STATUS_DORMANT);
    CHECK(info.exitStatus == 77);
    CHECK(sceKernelDeleteThread(t) == 0);

    before = sceKernelGetProcessTimeWide();
    CHECK(sceKernelDelayThread(20000) == 0);
    after = sceKernelGetProcessTimeWide();
    CHECK(after - before >= 20000);
    CHECK(after - before < 1000000);
    CHECK(sceKernelDelayThread(0) == SCE_KERNEL_ERROR_INVALID_ARGUMENT);
}

int main(void) {
    test_lw_mutex_counter();
    test_semaphores();
    test_event_flag();
    test_priority_order();
    test_cond();
    test_atomics();
    test_exit_and_delay();
    printf("threads: %d failed\n", g_failures);
    return g_failures;
}
