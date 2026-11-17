#include "shell.h"
#include "vfs.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"
#include "fb.h"
#include "string.h"

extern void shell_request_poweroff(void);
extern void shell_print_history(void);

static void print_node_line(const struct vfs_node *n, bool long_fmt) {
    if (long_fmt) {
        char type_ch = (n->type == VFS_DIR) ? 'd' : '-';
        const char *perm = (n->type == VFS_DIR) ? "rwxr-xr-x" : "rw-r--r--";
        kprintf("%c%s  1 root root %8lu  %s%s%s\n",
                type_ch, perm, n->size,
                (n->type == VFS_DIR) ? ANSI_BLUE : "",
                n->name,
                (n->type == VFS_DIR) ? ANSI_RESET "/" : "");
    } else {
        kprintf("%s%s%s  ",
                (n->type == VFS_DIR) ? ANSI_BLUE : "",
                n->name,
                (n->type == VFS_DIR) ? ANSI_RESET "/" : "");
    }
}

static void cmd_ls(int argc, char **argv) {
    bool long_fmt = false;
    const char *target = ".";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-la") == 0 || strcmp(argv[i], "-al") == 0) {
            long_fmt = true;
        } else {
            target = argv[i];
        }
    }

    struct vfs_node *n = vfs_resolve(target);
    if (!n) {
        kprintf("ls: cannot access '%s': No such file or directory\n", target);
        return;
    }
    if (n->type == VFS_FILE) {
        print_node_line(n, long_fmt);
        if (!long_fmt) kprintf("\n");
        return;
    }

    struct vfs_node *c = n->first_child;
    if (!c) {
        if (!long_fmt) kprintf("(empty)\n");
        return;
    }
    while (c) {
        print_node_line(c, long_fmt);
        c = c->next_sibling;
    }
    if (!long_fmt) kprintf("\n");
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: cat <file>\n");
        return;
    }
    struct vfs_node *n = vfs_resolve(argv[1]);
    if (!n) {
        kprintf("cat: %s: No such file or directory\n", argv[1]);
        return;
    }
    if (n->type == VFS_DIR) {
        kprintf("cat: %s: Is a directory\n", argv[1]);
        return;
    }
    for (size_t i = 0; i < n->size; i++) {
        uart_putc((char)n->data[i]);
    }
    if (n->size > 0 && n->data[n->size - 1] != '\n') uart_putc('\n');
}

static void cmd_echo(int argc, char **argv) {
    int redir_idx = -1;
    bool append = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], ">") == 0) {
            redir_idx = i;
            append = false;
            break;
        }
        if (strcmp(argv[i], ">>") == 0) {
            redir_idx = i;
            append = true;
            break;
        }
    }

    if (redir_idx > 0 && redir_idx + 1 < argc) {
        char out[512];
        out[0] = '\0';
        for (int i = 1; i < redir_idx; i++) {
            if (i > 1) strcat(out, " ");
            if (strlen(out) + strlen(argv[i]) + 2 < sizeof(out)) {
                strcat(out, argv[i]);
            }
        }
        strcat(out, "\n");
        if (vfs_write(argv[redir_idx + 1], out, strlen(out), append) != 0) {
            kprintf("echo: failed to write '%s'\n", argv[redir_idx + 1]);
        }
        return;
    }

    for (int i = 1; i < argc; i++) {
        kprintf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    kprintf("\n");
}

static void cmd_write(int argc, char **argv, bool append) {
    if (argc < 3) {
        kprintf("Usage: %s <file> <text...>\n", append ? "append" : "write");
        return;
    }
    char buf[512];
    buf[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2 && strlen(buf) + 1 < sizeof(buf)) strcat(buf, " ");
        if (strlen(buf) + strlen(argv[i]) + 2 < sizeof(buf)) strcat(buf, argv[i]);
    }
    strcat(buf, "\n");
    int rc = vfs_write(argv[1], buf, strlen(buf), append);
    if (rc != 0) {
        kprintf("%s: error %d writing '%s'\n", argv[0], rc, argv[1]);
    }
}

static void cmd_grep(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: grep <pattern> <file>\n");
        return;
    }
    struct vfs_node *n = vfs_resolve(argv[2]);
    if (!n || n->type != VFS_FILE) {
        kprintf("grep: %s: No such file\n", argv[2]);
        return;
    }
    const char *pat = argv[1];
    char line[256];
    size_t pos = 0;
    int line_no = 1;
    for (size_t i = 0; i <= n->size; i++) {
        char c = (i < n->size) ? (char)n->data[i] : '\n';
        if (c == '\n') {
            line[pos] = '\0';
            if (pos > 0 && strstr(line, pat)) {
                kprintf(ANSI_GREEN "%3d:" ANSI_RESET " %s\n", line_no, line);
            }
            pos = 0;
            line_no++;
        } else if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
        }
    }
}

static void cmd_wc(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: wc <file>\n");
        return;
    }
    struct vfs_node *n = vfs_resolve(argv[1]);
    if (!n || n->type != VFS_FILE) {
        kprintf("wc: %s: No such file\n", argv[1]);
        return;
    }
    size_t lines = 0, words = 0;
    bool in_word = false;
    for (size_t i = 0; i < n->size; i++) {
        char c = (char)n->data[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    kprintf(" %6lu %6lu %6lu %s\n", lines, words, n->size, argv[1]);
}

static void cmd_head_tail(int argc, char **argv, bool tail) {
    if (argc < 2) {
        kprintf("Usage: %s <file>\n", tail ? "tail" : "head");
        return;
    }
    struct vfs_node *n = vfs_resolve(argv[1]);
    if (!n || n->type != VFS_FILE) {
        kprintf("%s: %s: No such file\n", tail ? "tail" : "head", argv[1]);
        return;
    }
    size_t total_lines = 0;
    for (size_t i = 0; i < n->size; i++) {
        if (n->data[i] == '\n') total_lines++;
    }
    if (n->size > 0 && n->data[n->size - 1] != '\n') total_lines++;

    size_t start_line = (tail && total_lines > 10) ? (total_lines - 10) : 0;
    size_t end_line = tail ? total_lines : 10;
    size_t cur_line = 0;
    for (size_t i = 0; i < n->size; i++) {
        if (cur_line >= start_line && cur_line < end_line) {
            uart_putc((char)n->data[i]);
        }
        if (n->data[i] == '\n') cur_line++;
    }
    if (n->size > 0 && n->data[n->size - 1] != '\n') uart_putc('\n');
}

static void find_walk(struct vfs_node *dir, const char *pattern) {
    char path[VFS_PATH_MAX];
    vfs_get_path(dir, path, sizeof(path));
    if (!pattern || strstr(dir->name, pattern) || strcmp(dir->name, "/") == 0) {
        if (!pattern || strstr(dir->name, pattern)) kprintf("%s\n", path);
    }
    for (struct vfs_node *c = dir->first_child; c; c = c->next_sibling) {
        if (c->type == VFS_DIR) {
            find_walk(c, pattern);
        } else {
            if (!pattern || strstr(c->name, pattern)) {
                vfs_get_path(c, path, sizeof(path));
                kprintf("%s\n", path);
            }
        }
    }
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: hexdump <file>\n");
        return;
    }
    struct vfs_node *n = vfs_resolve(argv[1]);
    if (!n || n->type != VFS_FILE) {
        kprintf("hexdump: %s: No such file\n", argv[1]);
        return;
    }
    size_t limit = (n->size > 256U) ? 256U : n->size;
    for (size_t off = 0; off < limit; off += 16U) {
        kprintf("%08lx  ", off);
        for (size_t j = 0; j < 16U; j++) {
            if (off + j < limit) kprintf("%02x ", n->data[off + j]);
            else kprintf("   ");
        }
        kprintf(" |");
        for (size_t j = 0; j < 16U && off + j < limit; j++) {
            uint8_t ch = n->data[off + j];
            uart_putc((ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.');
        }
        kprintf("|\n");
    }
    if (n->size > limit) {
        kprintf("... (%lu total bytes, showing first 256 bytes)\n", n->size);
    }
}

static const char *task_state_str(enum task_state st) {
    switch (st) {
        case TASK_READY:    return "READY";
        case TASK_RUNNING:  return "RUN";
        case TASK_SLEEPING: return "SLEEP";
        case TASK_BLOCKED:  return "BLOCK";
        case TASK_ZOMBIE:   return "ZOMBIE";
        default:            return "UNUSED";
    }
}

static void cmd_ps(void) {
    size_t max_tasks = 0;
    struct task_struct *t = sched_get_task_table(&max_tasks);
    kprintf(ANSI_BOLD "  PID  STATE   PRIO  MLFQ  TICKS    SWITCHES  NAME\n" ANSI_RESET);
    for (size_t i = 0; i < max_tasks; i++) {
        if (t[i].state != TASK_UNUSED) {
            kprintf("  %-3d  %-6s  %-4u  Q%-3u  %-7lu  %-8lu  %s\n",
                    t[i].pid,
                    task_state_str(t[i].state),
                    t[i].priority,
                    t[i].mlfq_level,
                    t[i].total_ticks,
                    (uint64_t)t[i].switches,
                    t[i].name);
        }
    }
}

static void cmd_free(void) {
    struct pmm_stats ps;
    struct heap_stats hs;
    pmm_get_stats(&ps);
    heap_get_stats(&hs);

    kprintf("               total        used        free\n");
    kprintf("Phys RAM:  %6lu KB   %6lu KB   %6lu KB  (%lu/%lu 4KB pages)\n",
            (ps.total_pages * PAGE_SIZE) / 1024UL,
            (ps.used_pages * PAGE_SIZE) / 1024UL,
            (ps.free_pages * PAGE_SIZE) / 1024UL,
            ps.used_pages, ps.total_pages);
    kprintf("Kern Heap: %6lu KB   %6lu KB   %6lu KB  (%lu blocks)\n",
            hs.total_bytes / 1024UL,
            hs.used_bytes / 1024UL,
            hs.free_bytes / 1024UL,
            hs.block_count);
}

static void cmd_df(void) {
    struct vfs_stats vs;
    vfs_get_stats(&vs);
    kprintf("Filesystem     Inodes  IUsed  IFree  DataBytes  Mounted on\n");
    kprintf("ramfs          %6lu  %5lu  %5lu  %9lu  /\n",
            vs.max_inodes, vs.used_inodes,
            vs.max_inodes - vs.used_inodes,
            vs.used_bytes);
}

void shell_print_help(void) {
    kprintf(ANSI_CYAN ANSI_BOLD "FulmenOS Command Reference:\n" ANSI_RESET);
    kprintf(ANSI_YELLOW "  [Linux / File System Commands]\n" ANSI_RESET);
    kprintf("    ls [-l] [path]     cd <dir>           pwd               mkdir <dir>\n");
    kprintf("    touch <file>       cat <file>         nano <file>       write <f> <txt>\n");
    kprintf("    append <f> <txt>   echo <txt> [> f]   cp <src> <dst>     mv <src> <dst>\n");
    kprintf("    rm [-r] <path>     rmdir <dir>        stat <path>        grep <pat> <f>\n");
    kprintf("    wc <file>          head/tail <file>   find [name]        hexdump <file>\n");
    kprintf(ANSI_YELLOW "  [Process, Memory & System Commands]\n" ANSI_RESET);
    kprintf("    ps / threads / top kill <pid>         free              df\n");
    kprintf("    uname [-a]         uptime             whoami / hostname clear / history\n");
    kprintf("    gui / desktop      reboot             poweroff / exit\n");
    kprintf(ANSI_YELLOW "  [C/C++ User Space & Live Mini-C Interpreter]\n" ANSI_RESET);
    kprintf("    hello | fib [N] | primes [N] | calc   Run ELF64 C/C++ binaries in EL0\n");
    kprintf("    gcc | g++ | cc <file> [-o out]        Compile & run live C / C++ programs\n");
    kprintf("                                          (structs/classes/cout/cin/pointers)\n");
    kprintf(ANSI_YELLOW "  [OS Architecture Demos]\n" ANSI_RESET);
    kprintf("    sched [rr|prio|mlfq]                  Switch CPU scheduler policy live\n");
    kprintf("    sched_test                            Run multi-thread scheduler benchmark\n");
    kprintf("    mem | pagetable <hex> | alloc_test    PMM, TTBR0 MMU walk, & heap test\n");
    kprintf("    mutex_test | sem_test | deadlock_test Sync & Wait-For Graph deadlock guard\n");
    kprintf("    el0_test | test_all                   EL1<->EL0 trap demo & full test suite\n");
}

bool cmd_linux_dispatch(int argc, char **argv) {
    const char *cmd = argv[0];

    if (strcmp(cmd, "help") == 0) { shell_print_help(); return true; }
    if (strcmp(cmd, "ls") == 0) { cmd_ls(argc, argv); return true; }
    if (strcmp(cmd, "pwd") == 0) {
        char p[VFS_PATH_MAX];
        vfs_get_path(vfs_get_cwd(), p, sizeof(p));
        kprintf("%s\n", p);
        return true;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char *target = (argc > 1) ? argv[1] : "/home";
        int rc = vfs_chdir(target);
        if (rc != 0) kprintf("cd: %s: No such directory\n", target);
        return true;
    }
    if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 2) { kprintf("Usage: mkdir <dir>\n"); return true; }
        if (!vfs_mkdir(argv[1])) kprintf("mkdir: cannot create '%s'\n", argv[1]);
        return true;
    }
    if (strcmp(cmd, "rmdir") == 0 || strcmp(cmd, "rm") == 0) {
        bool rec = false;
        const char *target = NULL;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0) rec = true;
            else target = argv[i];
        }
        if (!target) { kprintf("Usage: %s [-r] <path>\n", cmd); return true; }
        int rc = vfs_remove(target, rec);
        if (rc != 0) kprintf("%s: cannot remove '%s' (err=%d)\n", cmd, target, rc);
        return true;
    }
    if (strcmp(cmd, "touch") == 0) {
        if (argc < 2) { kprintf("Usage: touch <file>\n"); return true; }
        if (!vfs_touch(argv[1])) kprintf("touch: cannot create '%s'\n", argv[1]);
        return true;
    }
    if (strcmp(cmd, "nano") == 0 || strcmp(cmd, "edit") == 0) {
        cmd_nano(argc, argv);
        return true;
    }
    if (strcmp(cmd, "gui") == 0 || strcmp(cmd, "desktop") == 0 || strcmp(cmd, "startx") == 0) {
        cmd_gui(argc, argv);
        return true;
    }
    if (strcmp(cmd, "install") == 0 || strcmp(cmd, "apt") == 0 || strcmp(cmd, "apt-get") == 0) {
        const char *pkg = (argc >= 3 && strcmp(argv[1], "install") == 0) ? argv[2]
                        : (argc >= 2) ? argv[1] : "";
        if (strcmp(pkg, "nano") == 0) {
            kprintf(ANSI_GREEN "[pkg]" ANSI_RESET " 'nano' (GNU nano 1.0 built-in for FulmenOS VFS) is installed and ready.\n");
            kprintf("Run: nano <file>  (e.g. nano /home/hello.c)\n");
        } else if (strcmp(pkg, "gcc") == 0 || strcmp(pkg, "g++") == 0 ||
                   strcmp(pkg, "build-essential") == 0) {
            kprintf(ANSI_GREEN "[pkg]" ANSI_RESET " 'gcc' (C17) and 'g++' (C++20) built-in compilers are ready.\n");
            kprintf("Usage: gcc file.c [-o out]  |  g++ app.cpp [-o out]\n");
        } else {
            kprintf("Built-in tools ready in FulmenOS: gcc, g++, cc, nano, hello, fib, primes, calc\n");
            kprintf("Usage: gcc <file.c> [-o out]  |  g++ <file.cpp> [-o out]  |  nano <file>\n");
        }
        return true;
    }
    if (strcmp(cmd, "cat") == 0) { cmd_cat(argc, argv); return true; }
    if (strcmp(cmd, "echo") == 0) { cmd_echo(argc, argv); return true; }
    if (strcmp(cmd, "write") == 0) { cmd_write(argc, argv, false); return true; }
    if (strcmp(cmd, "append") == 0) { cmd_write(argc, argv, true); return true; }
    if (strcmp(cmd, "cp") == 0) {
        if (argc < 3) { kprintf("Usage: cp <src> <dst>\n"); return true; }
        if (vfs_copy(argv[1], argv[2]) != 0) kprintf("cp: failed\n");
        return true;
    }
    if (strcmp(cmd, "mv") == 0) {
        if (argc < 3) { kprintf("Usage: mv <src> <dst>\n"); return true; }
        if (vfs_move(argv[1], argv[2]) != 0) kprintf("mv: failed\n");
        return true;
    }
    if (strcmp(cmd, "stat") == 0) {
        if (argc < 2) { kprintf("Usage: stat <path>\n"); return true; }
        struct vfs_node *n = vfs_resolve(argv[1]);
        if (!n) { kprintf("stat: '%s' not found\n", argv[1]); return true; }
        char full[VFS_PATH_MAX];
        vfs_get_path(n, full, sizeof(full));
        kprintf("  File: %s\n  Type: %s | Inode: %u | Size: %lu bytes | Mode: 0%o\n",
                full, (n->type == VFS_DIR) ? "directory" : "regular file",
                n->id, n->size, n->mode);
        return true;
    }
    if (strcmp(cmd, "grep") == 0) { cmd_grep(argc, argv); return true; }
    if (strcmp(cmd, "wc") == 0) { cmd_wc(argc, argv); return true; }
    if (strcmp(cmd, "head") == 0) { cmd_head_tail(argc, argv, false); return true; }
    if (strcmp(cmd, "tail") == 0) { cmd_head_tail(argc, argv, true); return true; }
    if (strcmp(cmd, "find") == 0) {
        find_walk(vfs_get_root(), (argc > 1) ? argv[1] : NULL);
        return true;
    }
    if (strcmp(cmd, "hexdump") == 0) { cmd_hexdump(argc, argv); return true; }
    if (strcmp(cmd, "ps") == 0 || strcmp(cmd, "threads") == 0) { cmd_ps(); return true; }
    if (strcmp(cmd, "top") == 0) {
        cmd_free();
        kprintf("\n");
        cmd_ps();
        return true;
    }
    if (strcmp(cmd, "kill") == 0) {
        if (argc < 2) { kprintf("Usage: kill <pid>\n"); return true; }
        int rc = task_kill(atoi(argv[1]));
        if (rc != 0) kprintf("kill: error %d\n", rc);
        return true;
    }
    if (strcmp(cmd, "free") == 0) { cmd_free(); return true; }
    if (strcmp(cmd, "df") == 0) { cmd_df(); return true; }
    if (strcmp(cmd, "uname") == 0) {
        if (argc > 1 && strcmp(argv[1], "-a") == 0) {
            kprintf("FulmenOS fulmen 1.0.0 #1 SMP PREEMPT AArch64 ARMv8-A bare-metal\n");
        } else {
            kprintf("FulmenOS\n");
        }
        return true;
    }
    if (strcmp(cmd, "uptime") == 0) {
        uint64_t ms = timer_get_ms();
        kprintf("up %lu.%02lu seconds (%lu ticks @ %u Hz)\n",
                ms / 1000UL, (ms % 1000UL) / 10UL, timer_get_ticks(), TIMER_HZ);
        return true;
    }
    if (strcmp(cmd, "whoami") == 0) { kprintf("root\n"); return true; }
    if (strcmp(cmd, "hostname") == 0) { kprintf("fulmen\n"); return true; }
    if (strcmp(cmd, "date") == 0) {
        kprintf("Boot+%lu ms (ARM Generic Timer @ %lu Hz)\n", timer_get_ms(), timer_get_freq());
        return true;
    }
    if (strcmp(cmd, "clear") == 0) {
        kprintf("\033[2J\033[H");
        fb_clear();
        return true;
    }
    if (strcmp(cmd, "history") == 0) { shell_print_history(); return true; }
    if (strcmp(cmd, "reboot") == 0) {
        kprintf("FulmenOS: Rebooting via PSCI...\n");
        register uint64_t x0 __asm__("x0") = 0x84000009UL;
        __asm__ volatile("hvc #0" :: "r"(x0));
        return true;
    }
    if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "exit") == 0 || strcmp(cmd, "shutdown") == 0) {
        kprintf("FulmenOS: Powering off system cleanly. Goodbye!\n");
        shell_request_poweroff();
        register uint64_t x0 __asm__("x0") = 0x84000008UL;
        __asm__ volatile("hvc #0" :: "r"(x0));
        return true;
    }

    return false;
}
