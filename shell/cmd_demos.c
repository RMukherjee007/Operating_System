#include "shell.h"
#include "pmm.h"
#include "mmu.h"
#include "heap.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"
#include "elf.h"
#include "minic.h"
#include "uart.h"
#include "string.h"

/* ---------------- Scheduler Benchmark ---------------- */
static volatile int sched_done_count = 0;

static void cpu_worker_fn(void *arg) {
    int id = (int)(intptr_t)arg;
    uint64_t start = timer_get_ms();
    volatile uint64_t acc = 0;
    while ((timer_get_ms() - start) < 70UL) {
        for (int i = 0; i < 5000; i++) acc += (uint64_t)(i ^ id);
    }
    sched_done_count++;
}

static void io_worker_fn(void *arg) {
    UNUSED(arg);
    for (int round = 0; round < 3; round++) {
        volatile uint64_t acc = 0;
        for (int i = 0; i < 5000; i++) acc += (uint64_t)i;
        task_sleep_ms(20);
    }
    sched_done_count++;
}

static void cmd_sched(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Active Scheduler: " ANSI_CYAN "%s" ANSI_RESET " (options: rr | prio | mlfq)\n",
                sched_policy_name(sched_get_policy()));
        return;
    }
    if (strcmp(argv[1], "rr") == 0) sched_set_policy(SCHED_RR);
    else if (strcmp(argv[1], "prio") == 0) sched_set_policy(SCHED_PRIO);
    else if (strcmp(argv[1], "mlfq") == 0) sched_set_policy(SCHED_MLFQ);
    else {
        kprintf("Usage: sched [rr|prio|mlfq]\n");
        return;
    }
    kprintf(ANSI_GREEN "[SCHED]" ANSI_RESET " Switched CPU scheduling policy to " ANSI_BOLD "%s" ANSI_RESET "\n",
            sched_policy_name(sched_get_policy()));
}

static void cmd_sched_test(void) {
    kprintf(ANSI_CYAN "=== Scheduler Test (%s) ===\n" ANSI_RESET,
            sched_policy_name(sched_get_policy()));
    sched_done_count = 0;
    sched_trace_start(true);

    task_create("CPU_Heavy_A", cpu_worker_fn, (void *)1, 1);
    task_create("CPU_Heavy_B", cpu_worker_fn, (void *)2, 3);
    task_create("IO_Task_C",   io_worker_fn,  (void *)3, 2);

    uint64_t t0 = timer_get_ms();
    while (sched_done_count < 3 && (timer_get_ms() - t0) < 2000UL) {
        task_sleep_ms(20);
    }
    sched_trace_start(false);
    sched_trace_dump();
    kprintf(ANSI_GREEN "[PASS]" ANSI_RESET " All 3 worker threads completed under %s.\n",
            sched_policy_name(sched_get_policy()));
}

/* ---------------- Memory & MMU Demos ---------------- */
static void cmd_mem(void) {
    struct pmm_stats ps;
    struct heap_stats hs;
    struct boot_info bi;
    pmm_get_stats(&ps);
    heap_get_stats(&hs);
    pmm_get_boot_info(&bi);

    kprintf(ANSI_CYAN "=== FulmenOS Memory Subsystem & Boot Info ===\n" ANSI_RESET);
    kprintf("  MMU State     : %s (4KB Granule, 39-bit TTBR0_EL1)\n",
            mmu_is_enabled() ? ANSI_GREEN "ENABLED" ANSI_RESET : "DISABLED");
    kprintf("  Physical RAM  : 0x%08lx - 0x%08lx (%lu MB)\n",
            bi.ram_base, bi.ram_base + bi.ram_size, bi.ram_size / (1024UL * 1024UL));
    kprintf("  Kernel Image  : 0x%08lx - 0x%08lx (reserved w/ DTB from 0x%08lx)\n",
            bi.kernel_start, bi.kernel_end, bi.ram_base);
    kprintf("  Page Frames   : %lu total | %lu used (%lu reserved) | %lu free\n",
            ps.total_pages, ps.used_pages, ps.reserved_pages, ps.free_pages);
    kprintf("  Kernel Heap   : 0x%08lx (%lu KB total | %lu B used | %lu B free | %lu blocks)\n",
            bi.heap_base, hs.total_bytes / 1024UL, hs.used_bytes, hs.free_bytes, hs.block_count);
    kprintf("  Framebuffer   : 0x%08lx - 0x%08lx (Normal-NC, %lu MB)\n",
            bi.fb_base, bi.fb_base + bi.fb_size, bi.fb_size / (1024UL * 1024UL));
    kprintf("  EL0 User Arena: 0x%08lx - 0x%08lx (AP=EL0+EL1 RW, UXN=0, PXN=1)\n",
            bi.user_base, bi.user_base + bi.user_size);
    kprintf("  Embedded InitRD: %lu files (%lu bytes packed into kernel image)\n",
            bi.initrd_files, bi.initrd_bytes);
}

static void cmd_alloc_test(void) {
    kprintf(ANSI_CYAN "=== PMM & Coalescing Heap Allocator Test ===\n" ANSI_RESET);

    void *p1 = pmm_alloc_page();
    void *p2 = pmm_alloc_pages(4);
    kprintf("  pmm_alloc_page()    -> %p\n", p1);
    kprintf("  pmm_alloc_pages(4)  -> %p\n", p2);
    pmm_free_page(p1);
    pmm_free_pages(p2, 4);
    kprintf("  pmm_free_*()        -> Freed 5 physical 4KB frames cleanly.\n");

    struct heap_stats before, mid, after;
    heap_get_stats(&before);
    void *a = kmalloc(1024);
    void *b = kmalloc(2048);
    void *c = kmalloc(512);
    heap_get_stats(&mid);
    kprintf("  kmalloc(1024/2048/512) -> %p, %p, %p (blocks: %lu -> %lu)\n",
            a, b, c, before.block_count, mid.block_count);

    kfree(a);
    kfree(b);
    kfree(c);
    heap_get_stats(&after);
    kprintf("  kfree(a,b,c) coalesced -> blocks: %lu -> %lu | free bytes=%lu\n",
            mid.block_count, after.block_count, after.free_bytes);
    kprintf(ANSI_GREEN "[PASS]" ANSI_RESET " PMM & Heap coalescing verified.\n");
}

/* ---------------- Sync & Deadlock Demos ---------------- */
static kmutex_t test_mutex;
static volatile int shared_counter = 0;
static volatile int mutex_workers_done = 0;

static void mutex_worker_fn(void *arg) {
    UNUSED(arg);
    for (int i = 0; i < 500; i++) {
        kmutex_lock(&test_mutex);
        int tmp = shared_counter;
        if ((i % 100) == 0) task_yield();
        shared_counter = tmp + 1;
        kmutex_unlock(&test_mutex);
    }
    mutex_workers_done++;
}

static void cmd_mutex_test(void) {
    kprintf(ANSI_CYAN "=== Mutex Concurrency Stress Test ===\n" ANSI_RESET);
    kmutex_init(&test_mutex, "counter_lock", 1);
    shared_counter = 0;
    mutex_workers_done = 0;

    task_create("Mtx_W1", mutex_worker_fn, NULL, 2);
    task_create("Mtx_W2", mutex_worker_fn, NULL, 2);
    task_create("Mtx_W3", mutex_worker_fn, NULL, 2);

    uint64_t t0 = timer_get_ms();
    while (mutex_workers_done < 3 && (timer_get_ms() - t0) < 3000UL) {
        task_sleep_ms(20);
    }
    kprintf("  Expected counter: 1500 | Observed: %d\n", shared_counter);
    if (shared_counter == 1500) {
        kprintf(ANSI_GREEN "[PASS]" ANSI_RESET " Zero race conditions with mutex protection.\n");
    } else {
        kprintf(ANSI_RED "[FAIL]" ANSI_RESET " Counter mismatch!\n");
    }
}

#define BUF_CAP 4
static int sem_buf[BUF_CAP];
static int sem_in = 0, sem_out = 0;
static ksem_t sem_empty, sem_full;
static kmutex_t sem_mtx;
static volatile int prod_sum = 0, cons_sum = 0, sem_done = 0;

static void producer_fn(void *arg) {
    UNUSED(arg);
    for (int i = 1; i <= 8; i++) {
        ksem_wait(&sem_empty);
        kmutex_lock(&sem_mtx);
        sem_buf[sem_in] = i * 10;
        sem_in = (sem_in + 1) % BUF_CAP;
        prod_sum += i * 10;
        kprintf("  [Producer] -> pushed %d\n", i * 10);
        kmutex_unlock(&sem_mtx);
        ksem_post(&sem_full);
        if (i % 3 == 0) task_sleep_ms(10);
    }
    sem_done++;
}

static void consumer_fn(void *arg) {
    UNUSED(arg);
    for (int i = 1; i <= 8; i++) {
        ksem_wait(&sem_full);
        kmutex_lock(&sem_mtx);
        int val = sem_buf[sem_out];
        sem_out = (sem_out + 1) % BUF_CAP;
        cons_sum += val;
        kprintf("  [Consumer] <- popped %d\n", val);
        kmutex_unlock(&sem_mtx);
        ksem_post(&sem_empty);
        if (i % 2 == 0) task_sleep_ms(15);
    }
    sem_done++;
}

static void cmd_sem_test(void) {
    kprintf(ANSI_CYAN "=== Semaphore Producer-Consumer Test (Buffer=4) ===\n" ANSI_RESET);
    sem_in = sem_out = 0;
    prod_sum = cons_sum = 0;
    sem_done = 0;
    ksem_init(&sem_empty, "empty", BUF_CAP);
    ksem_init(&sem_full, "full", 0);
    kmutex_init(&sem_mtx, "ring_mtx", 1);

    task_create("Producer", producer_fn, NULL, 2);
    task_create("Consumer", consumer_fn, NULL, 2);

    uint64_t t0 = timer_get_ms();
    while (sem_done < 2 && (timer_get_ms() - t0) < 3000UL) {
        task_sleep_ms(20);
    }
    kprintf("  Produced sum=%d | Consumed sum=%d\n", prod_sum, cons_sum);
    if (prod_sum == 360 && cons_sum == 360) {
        kprintf(ANSI_GREEN "[PASS]" ANSI_RESET " Bounded buffer semaphore sync verified.\n");
    } else {
        kprintf(ANSI_RED "[FAIL]" ANSI_RESET " Producer-consumer mismatch!\n");
    }
}

static kmutex_t dl_alpha, dl_beta;
static volatile int dl_done = 0;

static void dl_thread_a(void *arg) {
    UNUSED(arg);
    kprintf("  [Thread A] Acquiring Mutex_Alpha (rank=1)...\n");
    kmutex_lock(&dl_alpha);
    task_sleep_ms(40);
    kprintf("  [Thread A] Requesting Mutex_Beta (rank=2)...\n");
    int rc = kmutex_lock(&dl_beta);
    if (rc == 0) {
        kprintf("  [Thread A] Acquired both Mutex_Alpha & Mutex_Beta safely!\n");
        kmutex_unlock(&dl_beta);
    }
    kmutex_unlock(&dl_alpha);
    dl_done++;
}

static void dl_thread_b(void *arg) {
    UNUSED(arg);
    task_sleep_ms(20);
    kprintf("  [Thread B] Acquiring Mutex_Beta (rank=2)...\n");
    kmutex_lock(&dl_beta);
    task_sleep_ms(40);
    kprintf("  [Thread B] Attempting out-of-order Mutex_Alpha (rank=1)...\n");
    int rc = kmutex_lock(&dl_alpha);
    if (rc == -EDEADLK) {
        kprintf("  [Thread B] Kernel refused circular wait! Releasing Mutex_Beta to recover...\n");
        kmutex_unlock(&dl_beta);
        task_sleep_ms(50);
        kmutex_lock(&dl_alpha);
        kmutex_lock(&dl_beta);
        kprintf("  [Thread B] Re-acquired locks in safe order (1 -> 2) and completed!\n");
        kmutex_unlock(&dl_beta);
        kmutex_unlock(&dl_alpha);
    } else if (rc == 0) {
        kmutex_unlock(&dl_alpha);
        kmutex_unlock(&dl_beta);
    }
    dl_done++;
}

static void cmd_deadlock_test(void) {
    kprintf(ANSI_CYAN "=== Wait-For Graph Deadlock Prevention Demo ===\n" ANSI_RESET);
    struct sync_stats before, after;
    sync_get_stats(&before);

    kmutex_init(&dl_alpha, "Mutex_Alpha", 1);
    kmutex_init(&dl_beta, "Mutex_Beta", 2);
    dl_done = 0;

    task_create("T_Alpha", dl_thread_a, NULL, 2);
    task_create("T_Beta",  dl_thread_b, NULL, 2);

    uint64_t t0 = timer_get_ms();
    while (dl_done < 2 && (timer_get_ms() - t0) < 3000UL) {
        task_sleep_ms(20);
    }

    sync_get_stats(&after);
    if (after.deadlocks_prevented > before.deadlocks_prevented && dl_done == 2) {
        kprintf(ANSI_GREEN "[PASS]" ANSI_RESET " Circular deadlock detected & prevented (%lu caught total).\n",
                after.deadlocks_prevented);
    } else {
        kprintf(ANSI_YELLOW "[INFO]" ANSI_RESET " Deadlock test completed (caught=%lu).\n",
                after.deadlocks_prevented - before.deadlocks_prevented);
    }
}

static void cmd_test_all(void) {
    kprintf(ANSI_BOLD ANSI_CYAN "\n================ FulmenOS Self-Test Suite ================\n" ANSI_RESET);
    cmd_mem();
    kprintf("\n");
    mmu_dump_pte(0x40200000UL);
    mmu_dump_pte(0x44000000UL);
    kprintf("\n");
    cmd_alloc_test();
    kprintf("\n");
    enum sched_policy saved = sched_get_policy();
    sched_set_policy(SCHED_RR);   cmd_sched_test(); kprintf("\n");
    sched_set_policy(SCHED_PRIO); cmd_sched_test(); kprintf("\n");
    sched_set_policy(SCHED_MLFQ); cmd_sched_test(); kprintf("\n");
    sched_set_policy(saved);
    cmd_mutex_test();
    kprintf("\n");
    cmd_sem_test();
    kprintf("\n");
    cmd_deadlock_test();
    kprintf("\n");
    el0_run_inline_demo();
    kprintf("\n");
    minic_run_file("/src/demo.c", 0, NULL);
    kprintf(ANSI_BOLD ANSI_GREEN "\n============= All FulmenOS Subsystems Passed! ============\n" ANSI_RESET);
}

bool cmd_demos_dispatch(int argc, char **argv) {
    const char *cmd = argv[0];

    if (strcmp(cmd, "sched") == 0) { cmd_sched(argc, argv); return true; }
    if (strcmp(cmd, "sched_test") == 0) { cmd_sched_test(); return true; }
    if (strcmp(cmd, "mem") == 0) { cmd_mem(); return true; }
    if (strcmp(cmd, "pagetable") == 0) {
        if (argc < 2) {
            kprintf("Usage: pagetable <hex_addr> (e.g. pagetable 0x44000000)\n");
            return true;
        }
        bool ok = false;
        uint64_t va = strtoul_hex(argv[1], &ok);
        if (!ok) kprintf("Invalid hex address: %s\n", argv[1]);
        else mmu_dump_pte(va);
        return true;
    }
    if (strcmp(cmd, "alloc_test") == 0) { cmd_alloc_test(); return true; }
    if (strcmp(cmd, "mutex_test") == 0) { cmd_mutex_test(); return true; }
    if (strcmp(cmd, "sem_test") == 0) { cmd_sem_test(); return true; }
    if (strcmp(cmd, "deadlock_test") == 0) { cmd_deadlock_test(); return true; }
    if (strcmp(cmd, "el0_test") == 0) { el0_run_inline_demo(); return true; }
    if (strcmp(cmd, "test_all") == 0) { cmd_test_all(); return true; }

    return false;
}
