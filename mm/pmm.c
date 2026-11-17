#include "pmm.h"
#include "string.h"

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];

#define BITMAP_WORDS (TOTAL_PAGES / 64U)

static uint64_t pmm_bitmap[BITMAP_WORDS];
static uint64_t used_page_count = 0;
static uint64_t reserved_page_count = 0;
static struct boot_info g_boot_info;

static inline void bitmap_set(size_t idx) {
    pmm_bitmap[idx / 64U] |= (1UL << (idx % 64U));
}

static inline void bitmap_clear(size_t idx) {
    pmm_bitmap[idx / 64U] &= ~(1UL << (idx % 64U));
}

static inline bool bitmap_test(size_t idx) {
    return (pmm_bitmap[idx / 64U] & (1UL << (idx % 64U))) != 0;
}

static void pmm_reserve_range(uint64_t start_addr, uint64_t end_addr) {
    uint64_t s = ALIGN_DOWN(start_addr, PAGE_SIZE);
    uint64_t e = ALIGN_UP(end_addr, PAGE_SIZE);
    if (s < RAM_BASE) s = RAM_BASE;
    if (e > RAM_BASE + RAM_SIZE) e = RAM_BASE + RAM_SIZE;

    for (uint64_t addr = s; addr < e; addr += PAGE_SIZE) {
        size_t idx = (size_t)((addr - RAM_BASE) / PAGE_SIZE);
        if (!bitmap_test(idx)) {
            bitmap_set(idx);
            used_page_count++;
            reserved_page_count++;
        }
    }
}

void pmm_init(void) {
    memset(pmm_bitmap, 0, sizeof(pmm_bitmap));
    used_page_count = 0;
    reserved_page_count = 0;

    uint64_t kstart = (uint64_t)__kernel_start;
    uint64_t kend = ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE);

    memset(&g_boot_info, 0, sizeof(g_boot_info));
    g_boot_info.ram_base = RAM_BASE;
    g_boot_info.ram_size = RAM_SIZE;
    g_boot_info.dtb_reserved_end = kstart;
    g_boot_info.kernel_start = kstart;
    g_boot_info.kernel_end = kend;
    g_boot_info.heap_base = KERNEL_HEAP_BASE;
    g_boot_info.heap_size = KERNEL_HEAP_SIZE;
    g_boot_info.fb_base = FB_ARENA_BASE;
    g_boot_info.fb_size = FB_ARENA_SIZE;
    g_boot_info.user_base = USER_ARENA_BASE;
    g_boot_info.user_size = USER_ARENA_SIZE;

    /* Reserve DTB + boot area + loaded kernel image, .bss, page tables, and boot stack */
    pmm_reserve_range(RAM_BASE, kend);

    /* Reserve dedicated Kernel Heap, ramfb Framebuffer, and EL0 User Arena */
    pmm_reserve_range(KERNEL_HEAP_BASE, KERNEL_HEAP_BASE + KERNEL_HEAP_SIZE);
    pmm_reserve_range(FB_ARENA_BASE, FB_ARENA_BASE + FB_ARENA_SIZE);
    pmm_reserve_range(USER_ARENA_BASE, USER_ARENA_BASE + USER_ARENA_SIZE);
}

void *pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0 || count > TOTAL_PAGES) return NULL;

    size_t run = 0;
    size_t start = 0;
    for (size_t i = 0; i < TOTAL_PAGES; i++) {
        if (!bitmap_test(i)) {
            if (run == 0) start = i;
            run++;
            if (run == count) {
                for (size_t j = 0; j < count; j++) {
                    bitmap_set(start + j);
                }
                used_page_count += count;
                void *ptr = (void *)(RAM_BASE + start * PAGE_SIZE);
                memset(ptr, 0, count * PAGE_SIZE);
                return ptr;
            }
        } else {
            run = 0;
        }
    }
    return NULL;
}

void pmm_free_page(void *addr) {
    pmm_free_pages(addr, 1);
}

void pmm_free_pages(void *addr, size_t count) {
    uint64_t a = (uint64_t)addr;
    if (a < RAM_BASE || a >= RAM_BASE + RAM_SIZE) return;
    if ((a & (PAGE_SIZE - 1)) != 0) return;

    size_t start = (size_t)((a - RAM_BASE) / PAGE_SIZE);
    for (size_t j = 0; j < count && (start + j) < TOTAL_PAGES; j++) {
        if (bitmap_test(start + j)) {
            bitmap_clear(start + j);
            if (used_page_count > 0) used_page_count--;
        }
    }
}

void pmm_get_stats(struct pmm_stats *out) {
    if (!out) return;
    out->total_pages = TOTAL_PAGES;
    out->used_pages = used_page_count;
    out->free_pages = TOTAL_PAGES - used_page_count;
    out->reserved_pages = reserved_page_count;
}

void pmm_set_initrd_info(size_t files, size_t bytes) {
    g_boot_info.initrd_files = files;
    g_boot_info.initrd_bytes = bytes;
}

void pmm_get_boot_info(struct boot_info *out) {
    if (!out) return;
    *out = g_boot_info;
}
