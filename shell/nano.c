#include "shell.h"
#include "vfs.h"
#include "heap.h"
#include "uart.h"
#include "fb.h"
#include "timer.h"
#include "string.h"

#define NANO_MAX_LINES     128U
#define NANO_MAX_COLS      96U
#define NANO_SCREEN_COLS   80U
#define NANO_VIEW_ROWS     20U
#define NANO_GUTTER_COLS   4U
#define NANO_TEXT_COLS     (NANO_SCREEN_COLS - NANO_GUTTER_COLS)

#define NANO_FG_NORMAL     0x00E5E9F0U
#define NANO_BG_NORMAL     0x0010141BU
#define NANO_FG_BAR        0x0010141BU
#define NANO_BG_BAR        0x00E5E9F0U
#define NANO_FG_LINENO     0x0060A5FAU
#define NANO_FG_TILDE      0x0064748BU
#define NANO_FG_STATUS     0x00FACC15U
#define NANO_FG_CURSOR     0x0010141BU
#define NANO_BG_CURSOR     0x0038BDF8U

enum nano_key {
    NK_NONE = 0,
    NK_UP = 256,
    NK_DOWN,
    NK_RIGHT,
    NK_LEFT,
    NK_HOME,
    NK_END,
    NK_DEL,
    NK_ESC
};

struct nano_state {
    char filepath[VFS_PATH_MAX];
    char lines[NANO_MAX_LINES][NANO_MAX_COLS];
    uint16_t line_len[NANO_MAX_LINES];
    uint32_t num_lines;
    uint32_t cur_row;
    uint32_t cur_col;
    uint32_t row_offset;
    uint32_t col_offset;
    bool modified;
    bool has_cut;
    char cut_buf[NANO_MAX_COLS];
    uint16_t cut_len;
    char status_msg[NANO_SCREEN_COLS + 1];
};

static void nano_set_status(struct nano_state *st, const char *msg) {
    strncpy(st->status_msg, msg, NANO_SCREEN_COLS);
    st->status_msg[NANO_SCREEN_COLS] = '\0';
}

static void nano_load_file(struct nano_state *st, const char *path) {
    strncpy(st->filepath, path, sizeof(st->filepath) - 1U);
    st->filepath[sizeof(st->filepath) - 1U] = '\0';

    st->num_lines = 1U;
    st->line_len[0] = 0U;
    st->lines[0][0] = '\0';
    st->modified = false;

    struct vfs_node *n = vfs_resolve(path);
    if (!n) {
        nano_set_status(st, "[ New File ]");
        return;
    }

    uint32_t r = 0U;
    uint16_t c = 0U;
    for (size_t i = 0; i < n->size && r < NANO_MAX_LINES; i++) {
        char ch = (char)n->data[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            st->lines[r][c] = '\0';
            st->line_len[r] = c;
            r++;
            c = 0U;
            if (r < NANO_MAX_LINES) {
                st->lines[r][0] = '\0';
                st->line_len[r] = 0U;
            }
        } else if (c + 1U < NANO_MAX_COLS) {
            if (ch == '\t') {
                for (int s = 0; s < 4 && c + 1U < NANO_MAX_COLS; s++) {
                    st->lines[r][c++] = ' ';
                }
            } else if ((uint8_t)ch >= 0x20U && (uint8_t)ch < 0x7FU) {
                st->lines[r][c++] = ch;
            }
        }
    }

    if (r < NANO_MAX_LINES) {
        st->lines[r][c] = '\0';
        st->line_len[r] = c;
        /* Drop trailing empty line caused by final newline */
        st->num_lines = (r > 0U && c == 0U) ? r : (r + 1U);
    } else {
        st->num_lines = NANO_MAX_LINES;
    }

    char msg[NANO_SCREEN_COLS + 1];
    snprintf(msg, sizeof(msg), "[ Read %u lines ]", st->num_lines);
    nano_set_status(st, msg);
}

static int nano_save_file(struct nano_state *st) {
    size_t max_bytes = (size_t)st->num_lines * (NANO_MAX_COLS + 1U) + 1U;
    char *flat = (char *)kmalloc(max_bytes);
    if (!flat) {
        nano_set_status(st, "[ Error: Out of heap memory ]");
        return -ENOMEM;
    }

    size_t out = 0;
    for (uint32_t i = 0; i < st->num_lines; i++) {
        if (st->line_len[i] > 0U) {
            memcpy(flat + out, st->lines[i], st->line_len[i]);
            out += st->line_len[i];
        }
        if (i + 1U < st->num_lines || st->line_len[i] > 0U) {
            flat[out++] = '\n';
        }
    }

    int rc = vfs_write(st->filepath, flat, out, false);
    kfree(flat);

    if (rc == 0) {
        st->modified = false;
        char msg[NANO_SCREEN_COLS + 1];
        snprintf(msg, sizeof(msg), "[ Wrote %u lines (%lu bytes) ]",
                 st->num_lines, (unsigned long)out);
        nano_set_status(st, msg);
    } else {
        nano_set_status(st, "[ Error saving file to VFS ]");
    }
    return rc;
}

static void nano_scroll(struct nano_state *st) {
    if (st->cur_row < st->row_offset) {
        st->row_offset = st->cur_row;
    } else if (st->cur_row >= st->row_offset + NANO_VIEW_ROWS) {
        st->row_offset = st->cur_row - NANO_VIEW_ROWS + 1U;
    }

    if (st->cur_col < st->col_offset) {
        st->col_offset = st->cur_col;
    } else if (st->cur_col >= st->col_offset + NANO_TEXT_COLS) {
        st->col_offset = st->cur_col - NANO_TEXT_COLS + 1U;
    }
}

static void nano_uart_move(uint32_t row, uint32_t col) {
    char seq[24];
    snprintf(seq, sizeof(seq), "\033[%u;%uH", row + 1U, col + 1U);
    uart_puts(seq);
}

static void nano_draw_padded_line(uint32_t screen_row, const char *text,
                                  uint32_t fg, uint32_t bg, bool invert_uart) {
    nano_uart_move(screen_row, 0U);
    if (invert_uart) uart_puts("\033[7m");

    size_t len = strlen(text);
    for (uint32_t col = 0; col < NANO_SCREEN_COLS; col++) {
        char ch = (col < len) ? text[col] : ' ';
        uart_putc(ch);
        fb_put_cell(col, screen_row, ch, fg, bg);
    }

    if (invert_uart) uart_puts("\033[0m");
}

static void nano_draw_text_row(const struct nano_state *st, uint32_t screen_row, uint32_t line_idx) {
    nano_uart_move(screen_row, 0U);

    if (line_idx < st->num_lines) {
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%3u ", line_idx + 1U);
        uart_puts("\033[36m");
        uart_puts(prefix);
        uart_puts("\033[0m");
        for (uint32_t c = 0; c < NANO_GUTTER_COLS; c++) {
            fb_put_cell(c, screen_row, prefix[c], NANO_FG_LINENO, NANO_BG_NORMAL);
        }

        uint16_t llen = st->line_len[line_idx];
        for (uint32_t c = 0; c < NANO_TEXT_COLS; c++) {
            uint32_t text_col = st->col_offset + c;
            char ch = (text_col < llen) ? st->lines[line_idx][text_col] : ' ';
            uart_putc(ch);

            bool is_cur = (line_idx == st->cur_row && text_col == st->cur_col);
            uint32_t fg = is_cur ? NANO_FG_CURSOR : NANO_FG_NORMAL;
            uint32_t bg = is_cur ? NANO_BG_CURSOR : NANO_BG_NORMAL;
            fb_put_cell(NANO_GUTTER_COLS + c, screen_row, ch, fg, bg);
        }
    } else {
        const char *tilde = "  ~ ";
        uart_puts("\033[90m");
        uart_puts(tilde);
        for (uint32_t c = 0; c < NANO_GUTTER_COLS; c++) {
            fb_put_cell(c, screen_row, tilde[c], NANO_FG_TILDE, NANO_BG_NORMAL);
        }
        uart_puts("\033[0m");
        for (uint32_t c = NANO_GUTTER_COLS; c < NANO_SCREEN_COLS; c++) {
            uart_putc(' ');
            fb_put_cell(c, screen_row, ' ', NANO_FG_NORMAL, NANO_BG_NORMAL);
        }
    }
}

static void nano_render(struct nano_state *st) {
    nano_scroll(st);

    char header[NANO_SCREEN_COLS + 1];
    snprintf(header, sizeof(header), "  GNU nano 1.0 (FulmenOS)   File: %-34s %s",
             st->filepath, st->modified ? "Modified" : "        ");
    nano_draw_padded_line(0U, header, NANO_FG_BAR, NANO_BG_BAR, true);

    for (uint32_t sr = 0; sr < NANO_VIEW_ROWS; sr++) {
        nano_draw_text_row(st, 1U + sr, st->row_offset + sr);
    }

    char status[NANO_SCREEN_COLS + 1];
    snprintf(status, sizeof(status), "  %-60s Ln %u/%u Col %u",
             st->status_msg, st->cur_row + 1U, st->num_lines, st->cur_col + 1U);
    nano_draw_padded_line(NANO_VIEW_ROWS + 1U, status, NANO_FG_STATUS, NANO_BG_NORMAL, false);

    nano_draw_padded_line(
        NANO_VIEW_ROWS + 2U,
        "^G Help     ^O WriteOut  ^K Cut Line   ^A StartLn   ^S Save",
        NANO_FG_BAR, NANO_BG_BAR, true);
    nano_draw_padded_line(
        NANO_VIEW_ROWS + 3U,
        "^X Exit     ^U Paste Ln  ^D Del Char   ^E EndLn     Arrows Move",
        NANO_FG_BAR, NANO_BG_BAR, true);

    uint32_t cur_sr = 1U + (st->cur_row - st->row_offset);
    uint32_t cur_sc = NANO_GUTTER_COLS + (st->cur_col - st->col_offset);
    fb_set_cursor(cur_sc, cur_sr);
    nano_uart_move(cur_sr, cur_sc);
}

static int nano_wait_byte_timeout(uint32_t ms) {
    uint64_t start = timer_get_ms();
    while ((timer_get_ms() - start) < ms) {
        int ch = uart_getc_nonblock();
        if (ch >= 0) return ch;
        __asm__ volatile("wfi");
    }
    return -1;
}

static int nano_read_key(void) {
    unsigned char ch = (unsigned char)uart_getc();
    if (ch != 0x1B) {
        return (int)ch;
    }

    int seq0 = nano_wait_byte_timeout(30U);
    if (seq0 < 0) return NK_ESC;
    if (seq0 != '[' && seq0 != 'O') return NK_ESC;

    int seq1 = nano_wait_byte_timeout(30U);
    if (seq1 < 0) return NK_ESC;

    switch (seq1) {
        case 'A': return NK_UP;
        case 'B': return NK_DOWN;
        case 'C': return NK_RIGHT;
        case 'D': return NK_LEFT;
        case 'H': return NK_HOME;
        case 'F': return NK_END;
        case '3':
            (void)nano_wait_byte_timeout(30U); /* Consume trailing '~' */
            return NK_DEL;
        default:
            return NK_NONE;
    }
}

static void nano_clamp_col(struct nano_state *st) {
    uint16_t len = st->line_len[st->cur_row];
    if (st->cur_col > len) st->cur_col = len;
}

static void nano_insert_char(struct nano_state *st, char ch) {
    uint32_t r = st->cur_row;
    uint16_t len = st->line_len[r];
    if (len + 1U >= NANO_MAX_COLS) return;

    memmove(&st->lines[r][st->cur_col + 1U],
            &st->lines[r][st->cur_col],
            (size_t)(len - st->cur_col) + 1U);
    st->lines[r][st->cur_col] = ch;
    st->line_len[r] = (uint16_t)(len + 1U);
    st->cur_col++;
    st->modified = true;
}

static void nano_insert_newline(struct nano_state *st) {
    if (st->num_lines >= NANO_MAX_LINES) {
        nano_set_status(st, "[ Max lines reached ]");
        return;
    }
    uint32_t r = st->cur_row;
    for (uint32_t i = st->num_lines; i > r + 1U; i--) {
        memcpy(st->lines[i], st->lines[i - 1U], NANO_MAX_COLS);
        st->line_len[i] = st->line_len[i - 1U];
    }
    st->num_lines++;

    uint16_t tail_len = (uint16_t)(st->line_len[r] - st->cur_col);
    memcpy(st->lines[r + 1U], &st->lines[r][st->cur_col], tail_len + 1U);
    st->line_len[r + 1U] = tail_len;

    st->lines[r][st->cur_col] = '\0';
    st->line_len[r] = (uint16_t)st->cur_col;

    st->cur_row++;
    st->cur_col = 0U;
    st->modified = true;
}

static void nano_delete_char_at(struct nano_state *st, uint32_t r, uint32_t c) {
    uint16_t len = st->line_len[r];
    if (c < len) {
        memmove(&st->lines[r][c], &st->lines[r][c + 1U], (size_t)(len - c));
        st->line_len[r] = (uint16_t)(len - 1U);
        st->modified = true;
    } else if (r + 1U < st->num_lines) {
        uint16_t next_len = st->line_len[r + 1U];
        if ((uint32_t)len + next_len < NANO_MAX_COLS) {
            memcpy(&st->lines[r][len], st->lines[r + 1U], (size_t)next_len + 1U);
            st->line_len[r] = (uint16_t)(len + next_len);
            for (uint32_t i = r + 1U; i + 1U < st->num_lines; i++) {
                memcpy(st->lines[i], st->lines[i + 1U], NANO_MAX_COLS);
                st->line_len[i] = st->line_len[i + 1U];
            }
            st->num_lines--;
            st->modified = true;
        }
    }
}

static void nano_backspace(struct nano_state *st) {
    if (st->cur_col > 0U) {
        st->cur_col--;
        nano_delete_char_at(st, st->cur_row, st->cur_col);
    } else if (st->cur_row > 0U) {
        uint32_t prev_len = st->line_len[st->cur_row - 1U];
        if (prev_len + st->line_len[st->cur_row] < NANO_MAX_COLS) {
            st->cur_row--;
            st->cur_col = prev_len;
            nano_delete_char_at(st, st->cur_row, prev_len);
        }
    }
}

static void nano_cut_line(struct nano_state *st) {
    uint32_t r = st->cur_row;
    memcpy(st->cut_buf, st->lines[r], (size_t)st->line_len[r] + 1U);
    st->cut_len = st->line_len[r];
    st->has_cut = true;

    if (st->num_lines == 1U) {
        st->lines[0][0] = '\0';
        st->line_len[0] = 0U;
        st->cur_col = 0U;
    } else {
        for (uint32_t i = r; i + 1U < st->num_lines; i++) {
            memcpy(st->lines[i], st->lines[i + 1U], NANO_MAX_COLS);
            st->line_len[i] = st->line_len[i + 1U];
        }
        st->num_lines--;
        if (st->cur_row >= st->num_lines) st->cur_row = st->num_lines - 1U;
        nano_clamp_col(st);
    }
    st->modified = true;
    nano_set_status(st, "[ Cut 1 line ]");
}

static void nano_paste_line(struct nano_state *st) {
    if (!st->has_cut) {
        nano_set_status(st, "[ Cut buffer empty ]");
        return;
    }
    if (st->num_lines >= NANO_MAX_LINES) {
        nano_set_status(st, "[ Max lines reached ]");
        return;
    }
    for (uint32_t i = st->num_lines; i > st->cur_row; i--) {
        memcpy(st->lines[i], st->lines[i - 1U], NANO_MAX_COLS);
        st->line_len[i] = st->line_len[i - 1U];
    }
    memcpy(st->lines[st->cur_row], st->cut_buf, (size_t)st->cut_len + 1U);
    st->line_len[st->cur_row] = st->cut_len;
    st->num_lines++;
    st->cur_row++;
    nano_clamp_col(st);
    st->modified = true;
    nano_set_status(st, "[ Pasted line ]");
}

static bool nano_confirm_exit(struct nano_state *st) {
    if (!st->modified) return true;
    nano_set_status(st, "Save modified buffer? (Y=Yes, N=No, ^C=Cancel)");
    nano_render(st);

    for (;;) {
        int key = nano_read_key();
        if (key == 'y' || key == 'Y') {
            return nano_save_file(st) == 0;
        }
        if (key == 'n' || key == 'N') {
            return true;
        }
        if (key == 0x03 || key == NK_ESC) {
            nano_set_status(st, "[ Cancelled ]");
            return false;
        }
    }
}

void cmd_nano(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : "/home/untitled.txt";
    struct vfs_node *existing = vfs_resolve(path);
    if (existing && existing->type == VFS_DIR) {
        kprintf("nano: '%s' is a directory\n", path);
        return;
    }

    struct nano_state *st = (struct nano_state *)kmalloc(sizeof(struct nano_state));
    if (!st) {
        kprintf("nano: out of memory\n");
        return;
    }
    memset(st, 0, sizeof(*st));
    nano_load_file(st, path);

    console_set_fb_mirror(false);
    uart_puts("\033[2J\033[H");
    fb_clear();

    bool running = true;
    while (running) {
        nano_render(st);
        int key = nano_read_key();

        switch (key) {
            case 0x18: /* Ctrl+X */
                if (nano_confirm_exit(st)) running = false;
                break;
            case 0x0F: /* Ctrl+O */
            case 0x13: /* Ctrl+S */
                nano_save_file(st);
                break;
            case 0x0B: /* Ctrl+K */
                nano_cut_line(st);
                break;
            case 0x15: /* Ctrl+U */
                nano_paste_line(st);
                break;
            case 0x07: /* Ctrl+G */
                nano_set_status(st, "^O/^S Save | ^X Exit | ^K Cut | ^U Paste | ^A/^E Line");
                break;
            case 0x01: /* Ctrl+A */
            case NK_HOME:
                st->cur_col = 0U;
                break;
            case 0x05: /* Ctrl+E */
            case NK_END:
                st->cur_col = st->line_len[st->cur_row];
                break;
            case 0x04: /* Ctrl+D */
            case NK_DEL:
                nano_delete_char_at(st, st->cur_row, st->cur_col);
                break;
            case NK_UP:
                if (st->cur_row > 0U) { st->cur_row--; nano_clamp_col(st); }
                break;
            case NK_DOWN:
                if (st->cur_row + 1U < st->num_lines) { st->cur_row++; nano_clamp_col(st); }
                break;
            case NK_LEFT:
                if (st->cur_col > 0U) {
                    st->cur_col--;
                } else if (st->cur_row > 0U) {
                    st->cur_row--;
                    st->cur_col = st->line_len[st->cur_row];
                }
                break;
            case NK_RIGHT:
                if (st->cur_col < st->line_len[st->cur_row]) {
                    st->cur_col++;
                } else if (st->cur_row + 1U < st->num_lines) {
                    st->cur_row++;
                    st->cur_col = 0U;
                }
                break;
            case '\r':
            case '\n':
                nano_insert_newline(st);
                break;
            case '\b':
            case 0x7F:
                nano_backspace(st);
                break;
            case '\t':
                for (int s = 0; s < 4; s++) nano_insert_char(st, ' ');
                break;
            default:
                if (key >= 0x20 && key < 0x7F) {
                    nano_insert_char(st, (char)key);
                }
                break;
        }
    }

    uart_puts("\033[0m\033[2J\033[H");
    console_set_fb_mirror(true);
    fb_clear();
    kfree(st);
}
