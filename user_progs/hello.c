#include "ulib.h"

int main(int argc, char **argv) {
    int pid = sys_getpid();
    int el = sys_getel();
    uint64_t up = sys_uptime();

    printf("[EL%d User Program] Hello from native AArch64 ELF binary!\n", el);
    printf("  -> PID=%d | Hardware Exception Level=EL%d | Uptime=%lu ms\n", pid, el, up);
    printf("  -> Program arguments (%d):", argc);
    for (int i = 0; i < argc; i++) {
        printf(" [%d]=%s", i, argv[i]);
    }
    printf("\n");

    char *dyn = (char *)malloc(64);
    if (dyn) {
        printf("  -> EL0 sys_sbrk() allocated user heap buffer at %p\n", dyn);
    }

    return 0;
}
