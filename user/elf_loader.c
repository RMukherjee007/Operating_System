#include "elf.h"
#include "vfs.h"
#include "pmm.h"
#include "mmu.h"
#include "sched.h"
#include "syscall.h"
#include "uart.h"
#include "string.h"

static void flush_icache_range(uint64_t start, uint64_t size) {
    uint64_t s = ALIGN_DOWN(start, 64);
    uint64_t e = ALIGN_UP(start + size, 64);
    for (uint64_t p = s; p < e; p += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("ic ialluis" ::: "memory");
    __asm__ volatile("dsb ish; isb" ::: "memory");
}

int elf_run_from_vfs(const char *path, int argc, char **argv, int *exit_code) {
    struct vfs_node *node = vfs_resolve(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_FILE) return -EISDIR;
    if (!node->data || node->size < sizeof(Elf64_Ehdr)) return -EINVAL;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)node->data;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        return -EINVAL;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_machine != EM_AARCH64) {
        return -EINVAL;
    }
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phoff > node->size ||
        eh->e_phnum > (node->size - eh->e_phoff) / sizeof(Elf64_Phdr)) {
        return -EINVAL;
    }

    uint64_t arena_end = USER_ARENA_BASE + USER_ARENA_SIZE;
    uint64_t stack_limit = arena_end - 0x20000UL; /* Reserve top 128 KB for EL0 stack */

    /* Clear EL0 arena before loading segments */
    memset((void *)USER_ARENA_BASE, 0, USER_ARENA_SIZE);

    uint64_t max_vaddr = USER_ARENA_BASE;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(node->data + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        if (ph->p_vaddr < USER_ARENA_BASE || ph->p_vaddr > stack_limit ||
            ph->p_memsz > stack_limit - ph->p_vaddr) {
            kprintf(ANSI_RED "[ELF] Segment out of EL0 arena bounds (0x%lx)\n" ANSI_RESET, ph->p_vaddr);
            return -EACCES;
        }
        if (ph->p_filesz > ph->p_memsz || ph->p_offset > node->size ||
            ph->p_filesz > node->size - ph->p_offset) {
            return -EINVAL;
        }

        if (ph->p_filesz > 0) {
            memcpy((void *)ph->p_vaddr, node->data + ph->p_offset, ph->p_filesz);
        }
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)(ph->p_vaddr + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
        }
        if (ph->p_vaddr + ph->p_memsz > max_vaddr) {
            max_vaddr = ph->p_vaddr + ph->p_memsz;
        }
    }

    if (eh->e_entry < USER_ARENA_BASE || eh->e_entry >= stack_limit) {
        return -EINVAL;
    }

    /* Configure sbrk heap right above loaded ELF segments */
    el0_set_sbrk_base(ALIGN_UP(max_vaddr + 4096UL, 16));

    /* Copy argv strings & pointer array onto EL0 stack (accessible to EL0) */
    uint64_t sp = arena_end - 16UL;
    uint64_t u_arg_ptrs[32];
    int safe_argc = (argc > 31) ? 31 : argc;

    for (int i = safe_argc - 1; i >= 0; i--) {
        size_t slen = strlen(argv[i]) + 1;
        sp -= slen;
        memcpy((void *)sp, argv[i], slen);
        u_arg_ptrs[i] = sp;
    }
    sp = ALIGN_DOWN(sp, 16);

    sp -= (uint64_t)(safe_argc + 1) * sizeof(uint64_t);
    sp = ALIGN_DOWN(sp, 16);
    uint64_t *u_argv = (uint64_t *)sp;
    for (int i = 0; i < safe_argc; i++) {
        u_argv[i] = u_arg_ptrs[i];
    }
    u_argv[safe_argc] = 0;

    flush_icache_range(USER_ARENA_BASE, max_vaddr - USER_ARENA_BASE + 4096UL);

    int rc = enter_el0(eh->e_entry, sp, (uint64_t)safe_argc, (uint64_t)u_argv);
    if (exit_code) *exit_code = rc;
    return 0;
}

int el0_run_inline_demo(void) {
    uint64_t cur_el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
    cur_el = (cur_el >> 2) & 0x3UL;

    kprintf(ANSI_CYAN "=== EL1 <-> EL0 Hardware Privilege Separation Demo ===\n" ANSI_RESET);
    kprintf("1. Kernel currently executing at: " ANSI_BOLD "EL%lu" ANSI_RESET "\n", cur_el);
    kprintf("2. Inspecting MMU permissions for EL0 user arena (0x%08lx):\n", USER_ARENA_BASE);
    mmu_dump_pte(USER_ARENA_BASE);
    kprintf("3. Dropping via ERET into EL0t to execute /bin/hello (syscalls via SVC #0)...\n\n");

    char *args[] = { "/bin/hello", "el0-demo", NULL };
    int rc = 0;
    int err = elf_run_from_vfs("/bin/hello", 2, args, &rc);
    if (err != 0) {
        kprintf(ANSI_RED "[FAIL] elf_run_from_vfs returned %d\n" ANSI_RESET, err);
        return err;
    }

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(cur_el));
    cur_el = (cur_el >> 2) & 0x3UL;
    kprintf("\n4. Returned cleanly via SYS_EXIT(%d) back to kernel " ANSI_BOLD "EL%lu" ANSI_RESET ".\n",
            rc, cur_el);
    return 0;
}
