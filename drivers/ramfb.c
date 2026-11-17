#include "fb.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include "string.h"

#define FW_CFG_FILE_DIR       0x0019U
#define FW_CFG_DMA_CTL_ERROR  0x01U
#define FW_CFG_DMA_CTL_SELECT 0x08U
#define FW_CFG_DMA_CTL_WRITE  0x10U

#define DRM_FORMAT_XRGB8888   0x34325258U

#define CONSOLE_COLS (FB_WIDTH / 8U)
#define CONSOLE_ROWS ((FB_HEIGHT - FB_HUD_HEIGHT) / 16U)

struct fw_cfg_file {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char name[56];
} __attribute__((packed));

struct fw_cfg_dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
} __attribute__((packed));

struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} __attribute__((packed));

static volatile struct fw_cfg_dma_access g_dma_req __attribute__((aligned(64)));
static struct ramfb_cfg g_ramfb_cfg __attribute__((aligned(64)));

static volatile uint32_t *const fb_pixels = (volatile uint32_t *)FB_PHYS_ADDR;
static volatile uint32_t *draw_target = (volatile uint32_t *)FB_PHYS_ADDR;
static bool fb_active = false;
static bool gui_mode_active = false;
static uint32_t cur_col = 0;
static uint32_t cur_row = 0;
static uint32_t fg_color = 0x00E5E9F0U;
static uint32_t bg_color = 0x0010141BU;

/* Simple ANSI escape parser for graphical console */
static int ansi_state = 0; /* 0 = normal, 1 = saw ESC, 2 = inside ESC[ */
static char ansi_buf[16];
static size_t ansi_len = 0;

static void cache_clean_range(const void *addr, size_t size) {
    uint64_t start = ALIGN_DOWN((uint64_t)addr, 64);
    uint64_t end = ALIGN_UP((uint64_t)addr + size, 64);
    for (uint64_t p = start; p < end; p += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint16_t fw_cfg_find_file(const char *target) {
    volatile uint16_t *ctl = (volatile uint16_t *)(FW_CFG_BASE + 0x08);
    volatile uint8_t *data = (volatile uint8_t *)(FW_CFG_BASE + 0x00);

    *ctl = __builtin_bswap16(FW_CFG_FILE_DIR);

    uint8_t cnt_bytes[4];
    for (int i = 0; i < 4; i++) cnt_bytes[i] = *data;
    uint32_t count = ((uint32_t)cnt_bytes[0] << 24) |
                     ((uint32_t)cnt_bytes[1] << 16) |
                     ((uint32_t)cnt_bytes[2] << 8) |
                     (uint32_t)cnt_bytes[3];
    if (count == 0 || count > 256U) return 0;

    for (uint32_t i = 0; i < count; i++) {
        struct fw_cfg_file f;
        uint8_t *raw = (uint8_t *)&f;
        for (size_t b = 0; b < sizeof(f); b++) raw[b] = *data;
        f.name[55] = '\0';
        if (strcmp(f.name, target) == 0) {
            return __builtin_bswap16(f.select);
        }
    }
    return 0;
}

void fb_set_render_target(uint32_t *buf) {
    draw_target = buf ? (volatile uint32_t *)buf : fb_pixels;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_active || x >= FB_WIDTH || y >= FB_HEIGHT) return;
    draw_target[y * FB_WIDTH + x] = color;
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t row = y; row < y + h && row < FB_HEIGHT; row++) {
        volatile uint32_t *line = draw_target + row * FB_WIDTH;
        for (uint32_t col = x; col < x + w && col < FB_WIDTH; col++) {
            line[col] = color;
        }
    }
}

void fb_draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t thickness, uint32_t color) {
    if (!fb_active || w == 0 || h == 0 || thickness == 0) return;
    if (thickness * 2U > w) thickness = w / 2U;
    if (thickness * 2U > h) thickness = h / 2U;
    fb_fill_rect(x, y, w, thickness, color);
    fb_fill_rect(x, y + h - thickness, w, thickness, color);
    fb_fill_rect(x, y, thickness, h, color);
    fb_fill_rect(x + w - thickness, y, thickness, h, color);
}

static void fb_draw_char_at(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg) {
    if (px + 8U > FB_WIDTH || py + 16U > FB_HEIGHT) return;
    uint8_t uc = (uint8_t)c;
    if (uc >= 128U) uc = '?';
    const uint8_t *glyph = g_font8x16[uc];
    for (uint32_t r = 0; r < 16U; r++) {
        uint8_t bits = glyph[r];
        volatile uint32_t *line = draw_target + (py + r) * FB_WIDTH + px;
        for (uint32_t col = 0; col < 8U; col++) {
            line[col] = (bits & (0x80U >> col)) ? fg : bg;
        }
    }
}

static const char *const mac_cursor_rows[18] = {
    "W...........",
    "WW..........",
    "WBW.........",
    "WBBW........",
    "WBBBW.......",
    "WBBBBW......",
    "WBBBBBW.....",
    "WBBBBBBW....",
    "WBBBBBBBW...",
    "WBBBBBBBBW..",
    "WBBBBBBBBBW.",
    "WBBBBBBWWWW.",
    "WBBBWBBW....",
    "WBBW.WBBW...",
    "WBW..WBBW...",
    "WW....WBBW..",
    "......WBBW..",
    ".......WW...",
};

static void fb_blit_cursor_hw(int32_t mx, int32_t my) {
    for (int32_t row = 0; row < 18; row++) {
        int32_t py = my + row;
        if (py < 0 || py >= (int32_t)FB_HEIGHT) continue;
        const char *pat = mac_cursor_rows[row];
        volatile uint32_t *line = fb_pixels + (uint32_t)py * FB_WIDTH;
        for (int32_t col = 0; col < 12; col++) {
            int32_t px = mx + col;
            if (px < 0 || px >= (int32_t)FB_WIDTH) continue;
            if (pat[col] == 'W') line[px] = 0x00FFFFFFU;
            else if (pat[col] == 'B') line[px] = 0x00000000U;
        }
    }
}

void fb_present_with_cursor(const uint32_t *backbuf, int32_t mx, int32_t my) {
    if (!fb_active || !backbuf) return;
    memcpy((void *)fb_pixels, backbuf, FB_WIDTH * FB_HEIGHT * FB_BPP);
    fb_blit_cursor_hw(mx, my);
}

void fb_move_cursor_only(const uint32_t *backbuf, int32_t old_mx, int32_t old_my,
                         int32_t new_mx, int32_t new_my) {
    if (!fb_active || !backbuf) return;
    for (int32_t r = 0; r < 18; r++) {
        int32_t py = old_my + r;
        if (py < 0 || py >= (int32_t)FB_HEIGHT) continue;
        const uint32_t *src = backbuf + (uint32_t)py * FB_WIDTH;
        volatile uint32_t *dst = fb_pixels + (uint32_t)py * FB_WIDTH;
        for (int32_t c = 0; c < 12; c++) {
            int32_t px = old_mx + c;
            if (px >= 0 && px < (int32_t)FB_WIDTH) {
                dst[px] = src[px];
            }
        }
    }
    fb_blit_cursor_hw(new_mx, new_my);
}

void fb_draw_char_px(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg) {
    if (!fb_active) return;
    fb_draw_char_at(px, py, c, fg, bg);
}

void fb_draw_string_px(uint32_t px, uint32_t py, const char *s, uint32_t fg, uint32_t bg) {
    if (!fb_active || !s) return;
    while (*s && px + 8U <= FB_WIDTH) {
        fb_draw_char_at(px, py, *s++, fg, bg);
        px += 8U;
    }
}

static void fb_scroll_up(void) {
    uint32_t top_y = FB_HUD_HEIGHT;
    uint32_t copy_rows = (CONSOLE_ROWS - 1U) * 16U;
    for (uint32_t r = 0; r < copy_rows; r++) {
        volatile uint32_t *dst = fb_pixels + (top_y + r) * FB_WIDTH;
        volatile uint32_t *src = fb_pixels + (top_y + r + 16U) * FB_WIDTH;
        for (uint32_t c = 0; c < FB_WIDTH; c++) {
            dst[c] = src[c];
        }
    }
    fb_fill_rect(0, top_y + (CONSOLE_ROWS - 1U) * 16U, FB_WIDTH, 16U, bg_color);
}

static void fb_apply_ansi(const char *seq) {
    if (strcmp(seq, "0") == 0 || strcmp(seq, "") == 0) {
        fg_color = 0x00E5E9F0U;
    } else if (strcmp(seq, "1") == 0) {
        fg_color = 0x00FFFFFFU;
    } else if (strcmp(seq, "31") == 0) {
        fg_color = 0x00F87171U;
    } else if (strcmp(seq, "32") == 0) {
        fg_color = 0x004ADE80U;
    } else if (strcmp(seq, "33") == 0) {
        fg_color = 0x00FACC15U;
    } else if (strcmp(seq, "34") == 0) {
        fg_color = 0x0060A5FAU;
    } else if (strcmp(seq, "35") == 0) {
        fg_color = 0x00C084FCU;
    } else if (strcmp(seq, "36") == 0) {
        fg_color = 0x0038BDF8U;
    } else if (strcmp(seq, "37") == 0) {
        fg_color = 0x00E5E9F0U;
    } else if (strcmp(seq, "2J") == 0) {
        fb_clear();
    }
}

#define FW_CFG_DMA_SIGNATURE  0x51454d5520434647ULL /* "QEMU CFG" */

bool fb_init(void) {
    /* Verify QEMU fw_cfg DMA interface availability before touching selector/DMA */
    uint64_t dma_sig = *(volatile uint64_t *)(FW_CFG_BASE + 0x10);
    if (dma_sig != __builtin_bswap64(FW_CFG_DMA_SIGNATURE)) {
        fb_active = false;
        return false;
    }

    uint16_t sel = fw_cfg_find_file("etc/ramfb");
    if (sel == 0) {
        fb_active = false;
        return false;
    }

    fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, bg_color);
    cache_clean_range((const void *)FB_PHYS_ADDR, FB_WIDTH * FB_HEIGHT * FB_BPP);

    g_ramfb_cfg.addr = __builtin_bswap64(FB_PHYS_ADDR);
    g_ramfb_cfg.fourcc = __builtin_bswap32(DRM_FORMAT_XRGB8888);
    g_ramfb_cfg.flags = 0;
    g_ramfb_cfg.width = __builtin_bswap32(FB_WIDTH);
    g_ramfb_cfg.height = __builtin_bswap32(FB_HEIGHT);
    g_ramfb_cfg.stride = __builtin_bswap32(FB_STRIDE);

    g_dma_req.control = __builtin_bswap32(((uint32_t)sel << 16) |
                                          FW_CFG_DMA_CTL_SELECT |
                                          FW_CFG_DMA_CTL_WRITE);
    g_dma_req.length = __builtin_bswap32((uint32_t)sizeof(g_ramfb_cfg));
    g_dma_req.address = __builtin_bswap64((uint64_t)&g_ramfb_cfg);

    cache_clean_range(&g_ramfb_cfg, sizeof(g_ramfb_cfg));
    cache_clean_range((const void *)&g_dma_req, sizeof(g_dma_req));

    *(volatile uint64_t *)(FW_CFG_BASE + 0x10) = __builtin_bswap64((uint64_t)&g_dma_req);
    __asm__ volatile("dsb sy" ::: "memory");

    for (uint32_t spin = 0; spin < 1000000U; spin++) {
        uint32_t ctrl = __builtin_bswap32(g_dma_req.control);
        if (ctrl == 0) break;
        if (ctrl & FW_CFG_DMA_CTL_ERROR) {
            fb_active = false;
            return false;
        }
    }

    fb_active = true;
    fb_clear();
    fb_update_hud();
    return true;
}

bool fb_is_active(void) {
    return fb_active;
}

void fb_set_gui_mode(bool enabled) {
    gui_mode_active = enabled;
}

bool fb_is_gui_mode(void) {
    return gui_mode_active;
}

static void fb_erase_cursor(void) {
    if (!fb_active || cur_col >= CONSOLE_COLS || cur_row >= CONSOLE_ROWS) return;
    fb_fill_rect(cur_col * 8U, FB_HUD_HEIGHT + cur_row * 16U, 8U, 16U, bg_color);
}

static void fb_draw_cursor(void) {
    if (!fb_active || cur_col >= CONSOLE_COLS || cur_row >= CONSOLE_ROWS) return;
    fb_fill_rect(cur_col * 8U, FB_HUD_HEIGHT + cur_row * 16U + 1U, 8U, 14U, 0x0038BDF8U);
}

void fb_clear(void) {
    if (!fb_active) return;
    fb_fill_rect(0, FB_HUD_HEIGHT, FB_WIDTH, FB_HEIGHT - FB_HUD_HEIGHT, bg_color);
    cur_col = 0;
    cur_row = 0;
    fb_update_hud();
    fb_draw_cursor();
}

void fb_update_hud(void) {
    if (!fb_active || gui_mode_active) return;
    uint32_t hud_bg = 0x001E293BU;
    uint32_t hud_fg = 0x0038BDF8U;
    uint32_t bar_line = 0x00334155U;

    fb_fill_rect(0, 0, FB_WIDTH, FB_HUD_HEIGHT - 2U, hud_bg);
    fb_fill_rect(0, FB_HUD_HEIGHT - 2U, FB_WIDTH, 2U, bar_line);

    struct pmm_stats ps;
    pmm_get_stats(&ps);
    uint64_t used_mb = (ps.used_pages * PAGE_SIZE) / (1024UL * 1024UL);
    uint64_t total_mb = (ps.total_pages * PAGE_SIZE) / (1024UL * 1024UL);

    char hud[128];
    snprintf(hud, sizeof(hud),
             " FULMEN OS AArch64 | EL1 | Sched: %-4s | Ticks: %-6lu | RAM: %lu/%lu MB",
             sched_policy_name(sched_get_policy()),
             timer_get_ticks(),
             used_mb,
             total_mb);

    fb_draw_string_px(4, 8, hud, hud_fg, hud_bg);
}

static void fb_newline(void) {
    cur_col = 0;
    cur_row++;
    if (cur_row >= CONSOLE_ROWS) {
        fb_scroll_up();
        cur_row = CONSOLE_ROWS - 1U;
    }
}

void fb_putc(char c) {
    if (!fb_active) return;

    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_len = 0;
            return;
        }
        ansi_state = 0;
    } else if (ansi_state == 2) {
        if ((c >= '0' && c <= '9') || c == ';') {
            if (ansi_len + 1 < sizeof(ansi_buf)) ansi_buf[ansi_len++] = c;
            return;
        }
        if (c == 'm' || c == 'J') {
            if (c == 'J' && ansi_len + 1 < sizeof(ansi_buf)) ansi_buf[ansi_len++] = 'J';
            ansi_buf[ansi_len] = '\0';
            fb_apply_ansi(ansi_buf);
        }
        ansi_state = 0;
        return;
    }

    if (c == '\033') {
        ansi_state = 1;
        return;
    }
    if (c == '\t') {
        uint32_t spaces = 4U - (cur_col % 4U);
        for (uint32_t i = 0; i < spaces; i++) fb_putc(' ');
        return;
    }
    if (c != '\r' && c != '\b' && c != '\n' && ((uint8_t)c < 0x20U || (uint8_t)c >= 0x7FU)) {
        return;
    }

    fb_erase_cursor();

    if (c == '\r') {
        cur_col = 0;
        fb_draw_cursor();
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            fb_draw_char_at(cur_col * 8U, FB_HUD_HEIGHT + cur_row * 16U, ' ', fg_color, bg_color);
        }
        fb_draw_cursor();
        return;
    }
    if (c == '\n') {
        fb_newline();
        fb_draw_cursor();
        return;
    }

    fb_draw_char_at(cur_col * 8U, FB_HUD_HEIGHT + cur_row * 16U, c, fg_color, bg_color);
    cur_col++;
    if (cur_col >= CONSOLE_COLS) {
        fb_newline();
    }
    fb_draw_cursor();
}

void fb_put_cell(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg) {
    if (!fb_active || col >= CONSOLE_COLS || row >= CONSOLE_ROWS) return;
    if ((uint8_t)c < 0x20U || (uint8_t)c >= 0x7FU) c = ' ';
    fb_draw_char_at(col * 8U, FB_HUD_HEIGHT + row * 16U, c, fg, bg);
}

void fb_set_cursor(uint32_t col, uint32_t row) {
    if (!fb_active) return;
    if (col >= CONSOLE_COLS) col = CONSOLE_COLS - 1U;
    if (row >= CONSOLE_ROWS) row = CONSOLE_ROWS - 1U;
    cur_col = col;
    cur_row = row;
    fb_fill_rect(cur_col * 8U, FB_HUD_HEIGHT + cur_row * 16U + 14U, 8U, 2U, 0x0038BDF8U);
}

void fb_puts(const char *s) {
    if (!s) return;
    while (*s) fb_putc(*s++);
}
