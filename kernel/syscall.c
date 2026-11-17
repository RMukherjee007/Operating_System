#include "syscall.h"
#include "irq.h"
#include "pmm.h"
#include "uart.h"
#include "sched.h"
#include "timer.h"

static uint64_t el0_sbrk_ptr = USER_ARENA_BASE + 0x100000UL;
static uint64_t el0_sbrk_limit = USER_ARENA_BASE + USER_ARENA_SIZE - 0x20000UL;

void el0_set_sbrk_base(uint64_t addr) {
    uint64_t aligned = ALIGN_UP(addr, 16);
    if (aligned < USER_ARENA_BASE) aligned = USER_ARENA_BASE;
    el0_sbrk_ptr = aligned;
}

static bool user_ptr_valid(uint64_t addr, size_t len) {
    if (addr < USER_ARENA_BASE) return false;
    if (addr + len < addr) return false;
    if (addr + len > USER_ARENA_BASE + USER_ARENA_SIZE) return false;
    return true;
}

void syscall_handler(struct pt_regs *regs) {
    uint64_t num = regs->regs[8];
    uint64_t a0 = regs->regs[0];
    uint64_t a1 = regs->regs[1];
    uint64_t a2 = regs->regs[2];

    switch (num) {
        case SYS_WRITE: {
            if (a0 != 1UL && a0 != 2UL) {
                regs->regs[0] = (uint64_t)-EINVAL;
                break;
            }
            size_t len = (size_t)a2;
            if (!user_ptr_valid(a1, len)) {
                regs->regs[0] = (uint64_t)-EACCES;
                break;
            }
            const char *buf = (const char *)a1;
            for (size_t i = 0; i < len; i++) {
                uart_putc(buf[i]);
            }
            regs->regs[0] = len;
            break;
        }

        case SYS_READ: {
            size_t max_len = (size_t)a2;
            if (max_len == 0 || !user_ptr_valid(a1, max_len)) {
                regs->regs[0] = (uint64_t)-EACCES;
                break;
            }
            char *buf = (char *)a1;
            size_t count = 0;
            irq_enable();
            while (count < max_len) {
                char c = uart_getc();
                if (c == '\r') c = '\n';
                buf[count++] = c;
                uart_putc(c);
                if (c == '\n') break;
            }
            irq_disable();
            regs->regs[0] = count;
            break;
        }

        case SYS_EXIT: {
            exit_el0((int)a0);
            break;
        }

        case SYS_YIELD: {
            task_yield();
            regs->regs[0] = 0;
            break;
        }

        case SYS_SLEEP: {
            task_sleep_ms(a0);
            regs->regs[0] = 0;
            break;
        }

        case SYS_GETPID: {
            regs->regs[0] = (uint64_t)sched_current()->pid;
            break;
        }

        case SYS_GETEL: {
            /* Return SPSR_EL1.M[3:2] from the trap frame (0 == EL0) */
            regs->regs[0] = (regs->spsr_el1 >> 2) & 0x3UL;
            break;
        }

        case SYS_UPTIME: {
            regs->regs[0] = timer_get_ms();
            break;
        }

        case SYS_SBRK: {
            int64_t inc = (int64_t)a0;
            uint64_t old = el0_sbrk_ptr;
            if (inc == 0) {
                regs->regs[0] = old;
                break;
            }
            uint64_t next = (inc > 0) ? (old + ALIGN_UP((uint64_t)inc, 16)) : old;
            if (next > el0_sbrk_limit || next < USER_ARENA_BASE) {
                regs->regs[0] = (uint64_t)-1;
                break;
            }
            el0_sbrk_ptr = next;
            regs->regs[0] = old;
            break;
        }

        default:
            regs->regs[0] = (uint64_t)-EINVAL;
            break;
    }
}
