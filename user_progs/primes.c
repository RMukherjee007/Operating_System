#include "ulib.h"

int main(int argc, char **argv) {
    int limit = (argc > 1) ? atoi(argv[1]) : 100;
    if (limit < 2) limit = 2;
    if (limit > 5000) limit = 5000;

    uint64_t t0 = sys_uptime();
    int count = 0;
    printf("[EL%d primes] Primes up to %d:\n  ", sys_getel(), limit);

    for (int n = 2; n <= limit; n++) {
        bool prime = true;
        for (int d = 2; d * d <= n; d++) {
            if (n % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) {
            if (count < 25) printf("%d ", n);
            count++;
        }
    }
    if (count > 25) printf("... ");
    uint64_t t1 = sys_uptime();
    printf("\n  Total primes found: %d (elapsed %lu ms)\n", count, (t1 - t0));
    return 0;
}
