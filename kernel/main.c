#include "types.h"
#include "uart.h"
#include "fb.h"
#include "virtio_kbd.h"
#include "gic.h"
#include "timer.h"
#include "irq.h"
#include "pmm.h"
#include "mmu.h"
#include "heap.h"
#include "sched.h"
#include "sync.h"
#include "vfs.h"
#include "shell.h"

static void shell_thread_entry(void *arg) {
    UNUSED(arg);
    shell_run();
}

static uint64_t read_current_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 0x3UL;
}

void kernel_main(void) {
    uart_init();
    pmm_init();
    fb_init();
    mmu_init();
    virtio_kbd_init();
    heap_init();
    gic_init();
    timer_init();
    uart_enable_irq();
    sched_init();
    sync_init();
    vfs_init();
    initrd_populate();

    kprintf("\n");
    kprintf(ANSI_CYAN ANSI_BOLD
            "======================================================================\n"
            "   ______      _                       ____   _____\n"
            "  |  ____|    | |                     / __ \\ / ____|\n"
            "  | |__  _   _| |_ __ ___   ___ _ __ | |  | | (___  \n"
            "  |  __|| | | | | '_ ` _ \\ / _ \\ '_ \\| |  | |\\___ \\ \n"
            "  | |   | |_| | | | | | | |  __/ | | | |__| |____) |\n"
            "  |_|    \\__,_|_|_| |_| |_|\\___|_| |_|\\____/|_____/  AArch64 Microkernel\n"
            "======================================================================\n"
            ANSI_RESET);

    struct pmm_stats ps;
    pmm_get_stats(&ps);
    struct vfs_stats vs;
    vfs_get_stats(&vs);

    kprintf(ANSI_GREEN "[OK]" ANSI_RESET " Booted cleanly in ARM Exception Level: " ANSI_BOLD "EL%lu" ANSI_RESET "\n",
            read_current_el());
    kprintf(ANSI_GREEN "[OK]" ANSI_RESET " PL011 UART @ 0x%08lx | Framebuffer (ramfb): %s\n",
            UART0_BASE, fb_is_active() ? "Active (800x600 XRGB8888)" : "Headless Serial");
    kprintf(ANSI_GREEN "[OK]" ANSI_RESET " PMM: %lu MB (%lu pages free) | Heap: %lu MB | MMU: %s\n",
            (ps.total_pages * PAGE_SIZE) / (1024UL * 1024UL),
            ps.free_pages,
            KERNEL_HEAP_SIZE / (1024UL * 1024UL),
            mmu_is_enabled() ? "Enabled (TTBR0_EL1)" : "Disabled");
    kprintf(ANSI_GREEN "[OK]" ANSI_RESET " GICv2 + Generic Timer (%lu Hz -> %u Hz OS tick)\n",
            timer_get_freq(), TIMER_HZ);
    kprintf(ANSI_GREEN "[OK]" ANSI_RESET " Scheduler: %s | Sync: Mutex/Sem + WFG Deadlock Guard\n",
            sched_policy_name(sched_get_policy()));
    kprintf(ANSI_GREEN "[OK]" ANSI_RESET " VFS RAM Disk: %lu files, %lu dirs mounted (/bin, /src, /etc, /home)\n",
            vs.file_count, vs.dir_count);
    if (fb_is_active() && virtio_kbd_device_count() > 0) {
        kprintf("\n" ANSI_CYAN "[BOOT]" ANSI_RESET " Starting FulmenOS Graphical Desktop...\n\n");
    } else {
        kprintf("\nType " ANSI_YELLOW "gui" ANSI_RESET " for the Graphical Desktop, "
                ANSI_YELLOW "help" ANSI_RESET " for commands, or "
                ANSI_YELLOW "test_all" ANSI_RESET " for self-tests.\n\n");
    }

    task_create("shell", shell_thread_entry, NULL, 4);

    irq_enable();
    schedule();

    for (;;) {
        sched_reap_zombies();
        __asm__ volatile("wfi");
    }
}
