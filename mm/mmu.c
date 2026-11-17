#include "mmu.h"
#include "pmm.h"
#include "uart.h"
#include "string.h"

#define RAM_L2_COUNT 64U /* 64 * 2MB = 128MB granular L3 mapping */

static uint64_t l1_table[512] __attribute__((aligned(4096)));
static uint64_t l2_ram_table[512] __attribute__((aligned(4096)));
static uint64_t l3_ram_tables[RAM_L2_COUNT][512] __attribute__((aligned(4096)));
static bool mmu_active = false;

static uint64_t make_page_desc(uint64_t phys_addr) {
    uint64_t attr = MT_NORMAL_WB;
    uint64_t ap = PTE_AP_RW_EL1;
    uint64_t xn = PTE_UXN; /* Kernel pages cannot be executed in EL0 */

    if (phys_addr >= FB_ARENA_BASE && phys_addr < (FB_ARENA_BASE + FB_ARENA_SIZE)) {
        attr = MT_NORMAL_NC;
        ap = PTE_AP_RW_EL1;
        xn = PTE_UXN | PTE_PXN;
    } else if (phys_addr >= USER_ARENA_BASE && phys_addr < (USER_ARENA_BASE + USER_ARENA_SIZE)) {
        attr = MT_NORMAL_WB;
        ap = PTE_AP_RW_ALL; /* Accessible to EL0 user mode */
        xn = PTE_PXN;       /* Executable in EL0 (UXN=0), protected from EL1 exec (PXN=1) */
    }

    return (phys_addr & ~0xFFFUL) |
           PTE_VALID |
           PTE_PAGE |
           PTE_AF |
           PTE_SH_INNER |
           PTE_ATTR(attr) |
           ap |
           xn;
}

void mmu_init(void) {
    memset(l1_table, 0, sizeof(l1_table));
    memset(l2_ram_table, 0, sizeof(l2_ram_table));
    memset(l3_ram_tables, 0, sizeof(l3_ram_tables));

    /* L1[0]: 0x00000000 - 0x3FFFFFFF (1 GB MMIO peripheral block, Device-nGnRnE) */
    l1_table[0] = 0x00000000UL |
                  PTE_VALID |
                  PTE_AF |
                  PTE_ATTR(MT_DEVICE_nGnRnE) |
                  PTE_AP_RW_EL1 |
                  PTE_UXN |
                  PTE_PXN;

    /* L1[1]: 0x40000000 - 0x7FFFFFFF -> L2 table */
    l1_table[1] = ((uint64_t)l2_ram_table) | PTE_VALID | PTE_TABLE;

    /* Populate L2[0..63] with L3 4KB page tables for 0x40000000 - 0x47FFFFFF */
    for (uint32_t l2_idx = 0; l2_idx < RAM_L2_COUNT; l2_idx++) {
        l2_ram_table[l2_idx] = ((uint64_t)l3_ram_tables[l2_idx]) | PTE_VALID | PTE_TABLE;
        for (uint32_t l3_idx = 0; l3_idx < 512U; l3_idx++) {
            uint64_t phys = RAM_BASE + ((uint64_t)l2_idx * 512UL + l3_idx) * PAGE_SIZE;
            l3_ram_tables[l2_idx][l3_idx] = make_page_desc(phys);
        }
    }

    /* Populate L2[64..127] (0x48000000 - 0x4FFFFFFF) as 2MB Normal WB blocks */
    for (uint32_t l2_idx = RAM_L2_COUNT; l2_idx < 128U; l2_idx++) {
        uint64_t phys = RAM_BASE + (uint64_t)l2_idx * (2UL * 1024UL * 1024UL);
        l2_ram_table[l2_idx] = phys |
                               PTE_VALID |
                               PTE_AF |
                               PTE_SH_INNER |
                               PTE_ATTR(MT_NORMAL_WB) |
                               PTE_AP_RW_EL1 |
                               PTE_UXN;
    }

    /* Configure MAIR_EL1: Attr0=Device-nGnRnE (0x00), Attr1=Normal-WB (0xFF), Attr2=Normal-NC (0x44) */
    uint64_t mair = (0x44UL << 16) | (0xFFUL << 8) | 0x00UL;
    __asm__ volatile("msr mair_el1, %0" :: "r"(mair));

    /* Configure TCR_EL1: T0SZ=25 (512GB / 39-bit VA), 4KB granule, Inner Shareable WB, IPS=40-bit, EPD1=1 */
    uint64_t tcr = (25UL << 0) |
                   (1UL << 8)  | /* IRGN0: WB WA */
                   (1UL << 10) | /* ORGN0: WB WA */
                   (3UL << 12) | /* SH0: Inner Shareable */
                   (0UL << 14) | /* TG0: 4KB */
                   (1UL << 23) | /* EPD1: Disable TTBR1_EL1 */
                   (2UL << 32);  /* IPS: 40-bit PA */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    /* Load TTBR0_EL1 with L1 table address */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1_table));
    __asm__ volatile("dsb ish; isb; tlbi vmalle1; dsb ish; isb" ::: "memory");

    /* Enable MMU (M=1), Data Cache (C=1), Instruction Cache (I=1); clear WXN (bit 19) & A (bit 1) */
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);
    sctlr &= ~((1UL << 1) | (1UL << 19));
    __asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr) : "memory");

    mmu_active = true;
}

bool mmu_is_enabled(void) {
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & 1UL) != 0;
}

static const char *attr_name(uint64_t desc) {
    uint64_t idx = (desc >> 2) & 7UL;
    if (idx == MT_DEVICE_nGnRnE) return "Device-nGnRnE";
    if (idx == MT_NORMAL_WB)     return "Normal-WB";
    if (idx == MT_NORMAL_NC)     return "Normal-NC";
    return "Unknown";
}

static const char *perm_name(uint64_t desc) {
    uint64_t ap = (desc >> 6) & 3UL;
    if (ap == 0) return "EL1 RW";
    if (ap == 1) return "EL0+EL1 RW";
    if (ap == 2) return "EL1 RO";
    return "EL0+EL1 RO";
}

void mmu_dump_pte(uint64_t vaddr) {
    uint64_t l1_idx = (vaddr >> 30) & 0x1FFUL;
    uint64_t l2_idx = (vaddr >> 21) & 0x1FFUL;
    uint64_t l3_idx = (vaddr >> 12) & 0x1FFUL;

    kprintf("MMU Translation Walk for VA " ANSI_CYAN "0x%016lx" ANSI_RESET ":\n", vaddr);
    kprintf("  Indices: L1=%lu, L2=%lu, L3=%lu\n", l1_idx, l2_idx, l3_idx);

    if (vaddr >= (1UL << 39)) {
        kprintf("  [FAULT] VA exceeds 39-bit TTBR0 range.\n");
        return;
    }

    uint64_t d1 = l1_table[l1_idx];
    kprintf("  L1[%lu] = 0x%016lx\n", l1_idx, d1);
    if (!(d1 & PTE_VALID)) {
        kprintf("  [UNMAPPED] L1 entry invalid.\n");
        return;
    }
    if ((d1 & 3UL) == 1UL) {
        kprintf("  -> 1GB Block: PA=0x%016lx | %s | %s | UXN=%lu PXN=%lu\n",
                (d1 & 0x0000FFFFC0000000UL) | (vaddr & 0x3FFFFFFFUL),
                attr_name(d1), perm_name(d1),
                (d1 >> 54) & 1UL, (d1 >> 53) & 1UL);
        return;
    }

    uint64_t *l2 = (uint64_t *)(d1 & 0x0000FFFFFFFFF000UL);
    uint64_t d2 = l2[l2_idx];
    kprintf("  L2[%lu] = 0x%016lx\n", l2_idx, d2);
    if (!(d2 & PTE_VALID)) {
        kprintf("  [UNMAPPED] L2 entry invalid.\n");
        return;
    }
    if ((d2 & 3UL) == 1UL) {
        kprintf("  -> 2MB Block: PA=0x%016lx | %s | %s | UXN=%lu PXN=%lu\n",
                (d2 & 0x0000FFFFFFE00000UL) | (vaddr & 0x1FFFFFUL),
                attr_name(d2), perm_name(d2),
                (d2 >> 54) & 1UL, (d2 >> 53) & 1UL);
        return;
    }

    uint64_t *l3 = (uint64_t *)(d2 & 0x0000FFFFFFFFF000UL);
    uint64_t d3 = l3[l3_idx];
    kprintf("  L3[%lu] = 0x%016lx\n", l3_idx, d3);
    if (!(d3 & PTE_VALID)) {
        kprintf("  [UNMAPPED] L3 entry invalid.\n");
        return;
    }
    kprintf("  -> 4KB Page:  PA=0x%016lx | %s | %s | UXN=%lu PXN=%lu\n",
            (d3 & 0x0000FFFFFFFFF000UL) | (vaddr & 0xFFFUL),
            attr_name(d3), perm_name(d3),
            (d3 >> 54) & 1UL, (d3 >> 53) & 1UL);
}
