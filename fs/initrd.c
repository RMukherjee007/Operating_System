#include "vfs.h"
#include "pmm.h"
#include "string.h"

struct initrd_entry {
    const char *path;
    const uint8_t *data;
    size_t size;
};

extern const struct initrd_entry g_initrd_entries[];
extern const size_t g_initrd_entry_count;

void initrd_populate(void) {
    vfs_mkdir("/bin");
    vfs_mkdir("/src");
    vfs_mkdir("/etc");
    vfs_mkdir("/home");
    vfs_mkdir("/tmp");

    const char *os_rel =
        "NAME=\"FulmenOS\"\n"
        "VERSION=\"1.0-aarch64\"\n"
        "ARCH=\"AArch64 (ARMv8-A)\"\n"
        "KERNEL=\"Fulmen Microkernel (EL1/EL0)\"\n";
    vfs_write("/etc/os-release", os_rel, strlen(os_rel), false);

    const char *motd =
        "Welcome to FulmenOS (AArch64 Bare-Metal Microkernel).\n"
        "Use 'help' for commands, 'ls /bin' for EL0 C/C++ programs, 'ls /src' for sources.\n";
    vfs_write("/etc/motd", motd, strlen(motd), false);

    const char *passwd = "root:x:0:0:root:/home:/bin/sh\n";
    vfs_write("/etc/passwd", passwd, strlen(passwd), false);

    const char *readme =
        "FulmenOS Quick Guide\n"
        "====================\n"
        "1. Run compiled C/C++ ELF programs in EL0 user space:\n"
        "     hello | fib 15 | primes | calc\n"
        "2. Write & run live C programs with the built-in Mini-C runner:\n"
        "     cc /src/demo.c\n"
        "3. Switch CPU schedulers & visualize execution:\n"
        "     sched rr | sched prio | sched mlfq\n"
        "     sched_test\n"
        "4. Inspect MMU page tables, heap, & deadlock prevention:\n"
        "     mem | pagetable 0x44000000 | alloc_test | deadlock_test\n";
    vfs_write("/home/readme.txt", readme, strlen(readme), false);

    const char *notes =
        "TODO:\n"
        "- Benchmark MLFQ vs Round Robin\n"
        "- Verify EL0 svc #0 syscall boundary\n"
        "- Test Wait-For Graph cycle detection\n";
    vfs_write("/home/notes.txt", notes, strlen(notes), false);

    size_t total_initrd_bytes = 0;
    for (size_t i = 0; i < g_initrd_entry_count; i++) {
        vfs_write(g_initrd_entries[i].path,
                  g_initrd_entries[i].data,
                  g_initrd_entries[i].size,
                  false);
        total_initrd_bytes += g_initrd_entries[i].size;
    }
    pmm_set_initrd_info(g_initrd_entry_count, total_initrd_bytes);

    vfs_chdir("/home");
}
