/* Files through libc's stdio and the kernel's own calls: a writable device (ux0:) and the read-only application
 * folder (app0:). */

#include <kernel.h>
#include <stdlib.h>
#include <string.h>

#include "program.h"

static void test_stdio(void) {
    FILE* f;
    char line[64];
    long size;
    CHECK(sceIoMkdir("ux0:data", 0777) == 0);
    f = fopen("ux0:data/notes.txt", "w");
    CHECK(f != NULL);
    if (!f) return;
    fprintf(f, "first %d\n", 1);
    fprintf(f, "second %s\n", "line");
    CHECK(fclose(f) == 0);

    f = fopen("ux0:data/notes.txt", "r");
    CHECK(f != NULL);
    if (!f) return;
    while (fgets(line, sizeof line, f)) printf("read: %s", line);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    size = ftell(f);
    printf("size: %ld\n", size);
    CHECK(fseek(f, 6, SEEK_SET) == 0);
    CHECK(fgetc(f) == '1');
    CHECK(fclose(f) == 0);

    f = fopen("ux0:data/notes.txt", "a");
    fputs("third\n", f);
    fclose(f);
    f = fopen("ux0:data/notes.txt", "rb");
    {
        char all[64];
        size_t n = fread(all, 1, sizeof all, f);
        printf("after append: %u bytes\n", (unsigned)n);
    }
    fclose(f);

    CHECK(rename("ux0:data/notes.txt", "ux0:data/renamed.txt") == 0);
    CHECK(fopen("ux0:data/notes.txt", "r") == NULL);
    CHECK(remove("ux0:data/renamed.txt") == 0);
}

static void test_kernel_io(void) {
    SceIoStat st;
    SceUID fd, dir;
    SceIoDirent entry;
    int names = 0;
    char buffer[16];
    /* app0: is the application's folder: readable, not writable. */
    CHECK(sceIoGetstat("app0:eboot.bin", &st) == 0);
    CHECK(SCE_STM_ISREG(st.st_mode));
    CHECK(st.st_size > 0);
    fd = sceIoOpen("app0:eboot.bin", SCE_O_RDONLY, 0);
    CHECK(fd > 0);
    CHECK(sceIoRead(fd, buffer, 4) == 4);
    printf("eboot.bin starts with SCE: %s\n", memcmp(buffer, "SCE", 3) == 0 ? "yes" : "no");
    CHECK(sceIoLseek(fd, 0, SCE_SEEK_END) == st.st_size);
    CHECK(sceIoPread(fd, buffer, 2, 1) == 2);
    CHECK(buffer[0] == 'C' && buffer[1] == 'E');
    CHECK(sceIoClose(fd) == 0);
    CHECK(sceIoOpen("app0:new.bin", SCE_O_WRONLY | SCE_O_CREAT, 0666) < 0);
    CHECK(sceIoOpen("app0:missing.bin", SCE_O_RDONLY, 0) == (SceUID)0x80010002); /* SCE_ERROR_ERRNO_ENOENT */

    dir = sceIoDopen("app0:sce_module");
    CHECK(dir > 0);
    memset(&entry, 0, sizeof entry);
    while (sceIoDread(dir, &entry) > 0) {
        if (strcmp(entry.d_name, "libc.suprx") == 0) ++names;
        memset(&entry, 0, sizeof entry);
    }
    CHECK(sceIoDclose(dir) == 0);
    printf("libc.suprx listed: %d\n", names);

    CHECK(sceIoRmdir("ux0:data") == 0);
}

int main(void) {
    test_stdio();
    test_kernel_io();
    printf("files: %d failed\n", g_failures);
    return g_failures;
}
