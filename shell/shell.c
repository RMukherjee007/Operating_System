#include "shell.h"
#include "uart.h"
#include "fb.h"
#include "virtio_kbd.h"
#include "vfs.h"
#include "elf.h"
#include "minic.h"
#include "sched.h"
#include "string.h"

#define SHELL_MAX_LINE 512U
#define SHELL_MAX_ARGS 32U
#define HISTORY_MAX    16U

static char cmd_history[HISTORY_MAX][SHELL_MAX_LINE];
static size_t history_count = 0;
static bool poweroff_requested = false;

bool shell_is_poweroff_requested(void) {
    return poweroff_requested;
}

void shell_request_poweroff(void) {
    poweroff_requested = true;
}

static void record_history(const char *line) {
    if (!line || !*line) return;
    if (history_count > 0 && strcmp(cmd_history[(history_count - 1) % HISTORY_MAX], line) == 0) {
        return;
    }
    strncpy(cmd_history[history_count % HISTORY_MAX], line, SHELL_MAX_LINE - 1);
    cmd_history[history_count % HISTORY_MAX][SHELL_MAX_LINE - 1] = '\0';
    history_count++;
}

void shell_print_history(void) {
    size_t start = (history_count > HISTORY_MAX) ? (history_count - HISTORY_MAX) : 0;
    for (size_t i = start; i < history_count; i++) {
        kprintf("  %3lu  %s\n", i + 1, cmd_history[i % HISTORY_MAX]);
    }
}

static int parse_args(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        char *dst = p;
        argv[argc++] = dst;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && quote == '"' && p[1]) {
                    p++;
                    if (*p == 'n') *dst++ = '\n';
                    else if (*p == 't') *dst++ = '\t';
                    else if (*p == '\\') *dst++ = '\\';
                    else if (*p == '"') *dst++ = '"';
                    else *dst++ = *p;
                    p++;
                } else {
                    *dst++ = *p++;
                }
            }
            if (*p == quote) p++;
            *dst = '\0';
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                *dst++ = *p++;
            }
            if (*p) *p++ = '\0';
            else *dst = '\0';
        }
    }
    argv[argc] = NULL;
    return argc;
}

static bool try_exec_elf(int argc, char **argv) {
    const char *prog = argv[0];
    char path[VFS_PATH_MAX];

    if (strcmp(prog, "run") == 0) {
        if (argc < 2) {
            kprintf("Usage: run </bin/prog | prog> [args...]\n");
            return true;
        }
        prog = argv[1];
        argc--;
        argv++;
    }

    struct vfs_node *n = NULL;
    if (prog[0] == '/' || prog[0] == '.') {
        strncpy(path, prog, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        n = vfs_resolve(path);
    } else {
        snprintf(path, sizeof(path), "/bin/%s", prog);
        n = vfs_resolve(path);
        if (!n || n->type != VFS_FILE) {
            strncpy(path, prog, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            n = vfs_resolve(path);
        }
    }

    if (!n || n->type != VFS_FILE || !n->data) {
        return false;
    }

    if (n->size >= 11U && memcmp(n->data, "#!fulmen-c\n", 11U) == 0) {
        minic_eval_source((const char *)n->data + 11U, path, argc, argv);
        return true;
    }

    if (n->size < 4U ||
        n->data[0] != 0x7F || n->data[1] != 'E' || n->data[2] != 'L' || n->data[3] != 'F') {
        return false;
    }

    int exit_code = 0;
    int err = elf_run_from_vfs(path, argc, argv, &exit_code);
    if (err != 0) {
        kprintf(ANSI_RED "Failed to execute '%s' (err=%d)\n" ANSI_RESET, path, err);
    } else if (exit_code != 0) {
        kprintf(ANSI_YELLOW "[EL0 process exited with code %d]\n" ANSI_RESET, exit_code);
    }
    return true;
}

static bool has_compiler_flag(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') return true;
    }
    return false;
}

void shell_exec_line(const char *raw_line) {
    if (!raw_line) return;
    char buf[SHELL_MAX_LINE];
    strncpy(buf, raw_line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[SHELL_MAX_ARGS];
    int argc = parse_args(buf, argv, SHELL_MAX_ARGS);
    if (argc == 0) return;

    if (cmd_demos_dispatch(argc, argv)) return;
    if (cmd_linux_dispatch(argc, argv)) return;

    if (strcmp(argv[0], "cc") == 0 && !has_compiler_flag(argc, argv)) {
        if (argc < 2) {
            kprintf("Usage: cc <file.c> [args...]\n");
            return;
        }
        int rc = minic_run_file(argv[1], argc - 1, &argv[1]);
        if (rc == -ENOENT) {
            kprintf(ANSI_RED "cc: %s: No such file\n" ANSI_RESET, argv[1]);
        }
        return;
    }

    if (strcmp(argv[0], "cc") == 0 || strcmp(argv[0], "gcc") == 0 ||
        strcmp(argv[0], "clang") == 0) {
        minic_compile_command(argc, argv, false);
        return;
    }

    if (strcmp(argv[0], "g++") == 0 || strcmp(argv[0], "c++") == 0 ||
        strcmp(argv[0], "clang++") == 0 || strcmp(argv[0], "cpp") == 0) {
        minic_compile_command(argc, argv, true);
        return;
    }

    if (try_exec_elf(argc, argv)) return;

    kprintf(ANSI_RED "fulmen: %s: command not found\n" ANSI_RESET, argv[0]);
}

void shell_run(void) {
    char line[SHELL_MAX_LINE];
    char cwd_path[VFS_PATH_MAX];

    /* Boot directly into GUI desktop if framebuffer and interactive input are active */
    if (fb_is_active() && virtio_kbd_device_count() > 0) {
        char *gui_argv[] = { "gui", NULL };
        cmd_gui(1, gui_argv);
        /* GUI exited — fall through to CLI shell as fallback */
    }

    while (!poweroff_requested) {
        vfs_get_path(vfs_get_cwd(), cwd_path, sizeof(cwd_path));
        kprintf(ANSI_GREEN "root@fulmen" ANSI_RESET ":" ANSI_BLUE "%s" ANSI_RESET "# ", cwd_path);

        size_t pos = 0;
        for (;;) {
            char c = uart_getc();
            if (c == '\r' || c == '\n') {
                uart_putc('\n');
                line[pos] = '\0';
                break;
            } else if (c == '\b' || (uint8_t)c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    uart_puts("\b \b");
                }
            } else if (c == 0x03) { /* Ctrl-C */
                uart_puts("^C\n");
                pos = 0;
                line[0] = '\0';
                break;
            } else if ((uint8_t)c >= 0x20 && pos + 1 < sizeof(line)) {
                line[pos++] = c;
                uart_putc(c);
            }
        }

        if (pos > 0) {
            record_history(line);
            shell_exec_line(line);
        }
        sched_reap_zombies();
    }
}
