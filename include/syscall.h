#ifndef FULMEN_SYSCALL_H
#define FULMEN_SYSCALL_H

#include "types.h"

/* System call numbers passed in x8 on svc #0 */
#define SYS_WRITE   1UL
#define SYS_READ    2UL
#define SYS_EXIT    3UL
#define SYS_YIELD   4UL
#define SYS_SLEEP   5UL
#define SYS_GETPID  6UL
#define SYS_GETEL   7UL
#define SYS_UPTIME  8UL
#define SYS_SBRK    9UL

#ifndef __ASSEMBLER__
struct pt_regs;
void syscall_handler(struct pt_regs *regs);
void el0_set_sbrk_base(uint64_t addr);
#endif

#endif /* FULMEN_SYSCALL_H */
