/* The standard streams: libc writes stdout and stderr to one tty; the lines come out in the order the program
 * writes them. */

#include "program.h"

int main(void) {
    printf("printf to stdout\n");
    fprintf(stderr, "fprintf to stderr\n");
    fputs("fputs to stdout\n", stdout);
    fflush(stdout);
    fputs("fputs to stderr\n", stderr);
    puts("puts to stdout");
    return 0;
}
