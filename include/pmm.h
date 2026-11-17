#ifndef FULMEN_PMM_H
#define FULMEN_PMM_H

#include "types.h"

#define PAGE_SIZE        4096UL
#define PAGE_SHIFT       12U

#define RAM_BASE         0x40000000UL
#define RAM_SIZE         (128UL * 1024UL * 1024UL) /* 128 MB managed by PMM */
#define TOTAL_PAGES      (RAM_SIZE / PAGE_SIZE)    /* 32,768 pages */

/* Reserved physical memory arenas */
#define KERNEL_HEAP_BASE 0x42000000UL
#define KERNEL_HEAP_SIZE (8UL * 1024UL * 1024UL)   /* 8 MB kernel heap */

#define FB_ARENA_BASE    0x43000000UL
#define FB_ARENA_SIZE    (2UL * 1024UL * 1024UL)   /* 2 MB ramfb framebuffer */

#define USER_ARENA_BASE  0x44000000UL
#define USER_ARENA_SIZE  (2UL * 1024UL * 1024UL)   /* 2 MB EL0 user execution arena */

struct pmm_stats {
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t reserved_pages;
};

struct boot_info {
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t dtb_reserved_end;
    uint64_t kernel_start;
    uint64_t kernel_end;
    uint64_t heap_base;
    uint64_t heap_size;
    uint64_t fb_base;
    uint64_t fb_size;
    uint64_t user_base;
    uint64_t user_size;
    size_t   initrd_files;
    size_t   initrd_bytes;
};

void pmm_init(void);
void *pmm_alloc_page(void);
void *pmm_alloc_pages(size_t count);
void pmm_free_page(void *addr);
void pmm_free_pages(void *addr, size_t count);
void pmm_get_stats(struct pmm_stats *out);
void pmm_set_initrd_info(size_t files, size_t bytes);
void pmm_get_boot_info(struct boot_info *out);

#endif /* FULMEN_PMM_H */
