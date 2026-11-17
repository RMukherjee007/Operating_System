#include <stdio.h>

int fact(int n) {
    if (n <= 1) return 1;
    return n * fact(n - 1);
}

int main() {
    printf("=== FulmenOS Live Mini-C Execution ===\n");
    for (int i = 1; i <= 7; i = i + 1) {
        printf("  fact(%d) = %d\n", i, fact(i));
    }
    return 0;
}
