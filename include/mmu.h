#ifndef FULMEN_MMU_H
#define FULMEN_MMU_H

#include "types.h"

/* MAIR_EL1 memory attribute indices */
#define MT_DEVICE_nGnRnE   0UL
#define MT_NORMAL_WB       1UL
#define MT_NORMAL_NC       2UL

/* ARMv8 Page/Block Descriptor bits */
#define PTE_VALID          (1UL << 0)
#define PTE_TABLE          (1UL << 1)
#define PTE_PAGE           (1UL << 1)
#define PTE_AF             (1UL << 10)
#define PTE_SH_INNER       (3UL << 8)
#define PTE_AP_RW_EL1      (0UL << 6)
#define PTE_AP_RW_ALL      (1UL << 6)
#define PTE_AP_RO_EL1      (2UL << 6)
#define PTE_AP_RO_ALL      (3UL << 6)
#define PTE_PXN            (1UL << 53)
#define PTE_UXN            (1UL << 54)
#define PTE_ATTR(idx)      (((uint64_t)(idx) & 7UL) << 2)

void mmu_init(void);
bool mmu_is_enabled(void);
void mmu_dump_pte(uint64_t vaddr);

#endif /* FULMEN_MMU_H */
