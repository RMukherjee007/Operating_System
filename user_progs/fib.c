#include "ulib.h"

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 15;
    if (n < 1) n = 1;
    if (n > 45) n = 45;

    printf("[EL%d fib] Computing first %d Fibonacci numbers:\n  ", sys_getel(), n);
    uint64_t a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        printf("%lu%s", a, (i + 1 < n) ? ", " : "\n");
        uint64_t next = a + b;
        a = b;
        b = next;
    }
    return 0;
}
