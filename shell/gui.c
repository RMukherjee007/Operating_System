#include "shell.h"
#include "fb.h"
#include "uart.h"
#include "virtio_kbd.h"
#include "pmm.h"
#include "heap.h"
#include "sched.h"
#include "vfs.h"
#include "timer.h"
#include "mmu.h"
#include "string.h"

/* Classic Macintosh System 7 / Mac OS 8 Platinum Palette (XRGB8888) */
#define MAC_WHITE         0x00FFFFFFU
#define MAC_BLACK         0x00000000U
#define MAC_MENUBAR_BG    0x00F6F6F8U
#define MAC_WIN_BG        0x00EAEAEFU
#define MAC_PLAT_LIGHT    0x00DCDCDEU
#define MAC_PLAT_MID      0x00C0C0C6U
#define MAC_PLAT_DARK     0x00808088U
#define MAC_PLAT_STRIPE   0x007C7C84U
#define MAC_NAVY          0x00000080U
#define MAC_TERM_BG       0x0010141CU
#define MAC_TERM_FG       0x00ECEFF4U
#define MAC_TERM_GREEN    0x0066E08AU
#define MAC_TERM_AMBER    0x00FFD166U
#define MAC_TERM_RED      0x00FF6B6BU
#define MAC_TERM_CYAN     0x0066D9EFU
#define MAC_PAPER_BG      0x00FFFFFFU
#define MAC_GUTTER_BG     0x00DCDCE2U

#define GUI_MENUBAR_H     22
#define GUI_TITLEBAR_H    20
#define GUI_TERM_CAP      4096U
#define GUI_CMD_MAX       128U
#define GUI_HIST_MAX      8U
#define GUI_ED_CAP        2048U

enum win_id {
    WIN_FINDER = 0,
    WIN_EDITOR = 1,
    WIN_TERM   = 2,
    WIN_ABOUT  = 3,
    WIN_COUNT  = 4
};

struct gui_win {
    int id;
    const char *title;
    int32_t x, y, w, h;
    int32_t norm_x, norm_y, norm_w, norm_h;
    bool visible;
    bool zoomed;
};

struct desktop_theme {
    uint32_t c1;
    uint32_t c2;
    uint32_t label_bg;
};

static const struct desktop_theme themes[3] = {
    { 0x006078A8U, 0x00546C9CU, 0x002D3E5EU }, /* Classic Mac Blue */
    { 0x00A8A8AEU, 0x00929298U, 0x00404046U }, /* System 7 Gray */
    { 0x003B8C8EU, 0x002E787AU, 0x001A484AU }, /* Bondi Teal */
};

static const char *const finder_dirs[5] = { "/home", "/src", "/bin", "/etc", "/tmp" };

static struct gui_win wins[WIN_COUNT];
static int z_order[WIN_COUNT]; /* z_order[WIN_COUNT - 1] is topmost window */
static int active_theme = 0;
static int active_menu = -1;
static int menu_hover_item = -1;
static int selected_icon = -1;

static uint32_t *backbuf = NULL;
static char gui_term_buf[GUI_TERM_CAP];
static size_t gui_term_len = 0;
static char cmd_buf[GUI_CMD_MAX];
static size_t cmd_len = 0;
static char cmd_hist[GUI_HIST_MAX][GUI_CMD_MAX];
static size_t cmd_hist_count = 0;
static int cmd_hist_pos = -1;

/* Finder state */
static size_t finder_dir_idx = 0;
static int finder_sel_idx = 0;
static int finder_scroll = 0;
static char finder_sel_path[VFS_PATH_MAX] = "";
static int finder_last_click_idx = -1;
static uint64_t finder_last_click_ms = 0;
static int new_file_seq = 1;

/* SimpleText / CodePad editor state */
static char ed_path[VFS_PATH_MAX] = "/home/hello.c";
static char ed_buf[GUI_ED_CAP];
static size_t ed_len = 0;
static size_t ed_cursor = 0;
static int ed_scroll_row = 0;
static bool ed_dirty = false;
static char ed_status[64] = "Ready (Ctrl+S Save, Ctrl+R Compile & Run)";

static bool gui_running = true;

/* -------------------------------------------------------------------------- */
/*  Pixel & Classic Macintosh Platinum Drawing Primitives                     */
/* -------------------------------------------------------------------------- */

static bool pt_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < (x + w) && py >= y && py < (y + h);
}

static void gui_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int32_t)FB_WIDTH || y >= (int32_t)FB_HEIGHT || w <= 0 || h <= 0) return;
    if (x + w > (int32_t)FB_WIDTH) w = (int32_t)FB_WIDTH - x;
    if (y + h > (int32_t)FB_HEIGHT) h = (int32_t)FB_HEIGHT - y;
    fb_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, col);
}

static void gui_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t col) {
    if (w <= 1 || h <= 1) return;
    gui_fill_rect(x, y, w, 1, col);
    gui_fill_rect(x, y + h - 1, w, 1, col);
    gui_fill_rect(x, y, 1, h, col);
    gui_fill_rect(x + w - 1, y, 1, h, col);
}

static void gui_draw_text_fit(int32_t x, int32_t y, int32_t max_cols,
                              const char *s, uint32_t fg, uint32_t bg, bool pad) {
    if (max_cols <= 0 || x < 0 || y < 0) return;
    int32_t i = 0;
    while (i < max_cols && s && s[i]) {
        if (x + i * 8 + 8 <= (int32_t)FB_WIDTH && y + 16 <= (int32_t)FB_HEIGHT) {
            fb_draw_char_px((uint32_t)(x + i * 8), (uint32_t)y, s[i], fg, bg);
        }
        i++;
    }
    if (pad) {
        while (i < max_cols) {
            if (x + i * 8 + 8 <= (int32_t)FB_WIDTH && y + 16 <= (int32_t)FB_HEIGHT) {
                fb_draw_char_px((uint32_t)(x + i * 8), (uint32_t)y, ' ', fg, bg);
            }
            i++;
        }
    }
}

static void gui_draw_bevel_box(int32_t x, int32_t y, int32_t w, int32_t h,
                               uint32_t fill, bool sunken) {
    gui_fill_rect(x, y, w, h, fill);
    gui_outline(x, y, w, h, MAC_BLACK);
    if (w > 4 && h > 4) {
        uint32_t tl = sunken ? MAC_PLAT_DARK : MAC_WHITE;
        uint32_t br = sunken ? MAC_WHITE : MAC_PLAT_DARK;
        gui_fill_rect(x + 1, y + 1, w - 2, 1, tl);
        gui_fill_rect(x + 1, y + 1, 1, h - 2, tl);
        gui_fill_rect(x + 1, y + h - 2, w - 2, 1, br);
        gui_fill_rect(x + w - 2, y + 1, 1, h - 2, br);
    }
}

static void gui_draw_mac_button(int32_t x, int32_t y, int32_t w, int32_t h,
                                const char *label, bool highlighted) {
    uint32_t bg = highlighted ? MAC_NAVY : MAC_WIN_BG;
    uint32_t fg = highlighted ? MAC_WHITE : MAC_BLACK;
    gui_draw_bevel_box(x, y, w, h, bg, highlighted);

    int32_t len = (int32_t)strlen(label);
    int32_t max_chars = (w - 6) / 8;
    if (max_chars < 1) return;
    if (len > max_chars) len = max_chars;
    int32_t tx = x + (w - len * 8) / 2;
    int32_t ty = y + (h - 16) / 2;
    if (ty < y + 1) ty = y + 1;
    gui_draw_text_fit(tx, ty, len, label, fg, bg, false);
}

static void gui_draw_radio(int32_t x, int32_t y, const char *label, bool checked) {
    gui_fill_rect(x, y + 2, 12, 12, MAC_WHITE);
    gui_outline(x, y + 2, 12, 12, MAC_BLACK);
    if (checked) {
        gui_fill_rect(x + 3, y + 5, 6, 6, MAC_NAVY);
    }
    gui_draw_text_fit(x + 16, y, 16, label, MAC_BLACK, MAC_WIN_BG, false);
}

static void gui_draw_progress(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint64_t used, uint64_t total, uint32_t col) {
    gui_draw_bevel_box(x, y, w, h, MAC_PLAT_LIGHT, true);
    if (total == 0 || w <= 6 || h <= 6) return;
    int32_t inner = w - 4;
    int32_t fw = (int32_t)((used * (uint64_t)inner) / total);
    if (used > 0 && fw < 3) fw = 3;
    if (fw > inner) fw = inner;
    gui_fill_rect(x + 2, y + 2, fw, h - 4, col);
}

/* -------------------------------------------------------------------------- */
/*  Desktop Background & Pixel-Art Icons                                      */
/* -------------------------------------------------------------------------- */

static void gui_draw_desktop_bg(void) {
    uint32_t c1 = themes[active_theme].c1;
    uint32_t c2 = themes[active_theme].c2;

    if (backbuf) {
        for (uint32_t y = GUI_MENUBAR_H; y < FB_HEIGHT; y++) {
            uint32_t *line = backbuf + y * FB_WIDTH;
            uint32_t phase = y & 1U;
            for (uint32_t x = 0; x < FB_WIDTH; x++) {
                line[x] = ((x ^ phase) & 1U) ? c2 : c1;
            }
        }
    } else {
        gui_fill_rect(0, GUI_MENUBAR_H, (int32_t)FB_WIDTH, (int32_t)FB_HEIGHT - GUI_MENUBAR_H, c1);
    }

    /* Classic Macintosh rounded CRT bottom corners */
    static const uint8_t corner[4] = { 4, 2, 1, 1 };
    for (int32_t r = 0; r < 4; r++) {
        int32_t span = corner[r];
        gui_fill_rect(0, (int32_t)FB_HEIGHT - 1 - r, span, 1, MAC_BLACK);
        gui_fill_rect((int32_t)FB_WIDTH - span, (int32_t)FB_HEIGHT - 1 - r, span, 1, MAC_BLACK);
    }
}

/* 16x16 palette-indexed sprites drawn 2x scaled to 32x32 */
static const char *const sprite_hd[16] = {
    "................", "..KKKKKKKKKKKK..", ".KWWWWWWWWWWWWK.", ".KWGGGGGGGGGGWK.",
    ".KWGKKKKKKKKGWK.", ".KWGKBBBBBBKGWK.", ".KWGGGGGGGGGGWK.", ".KWGGGGGGGGGGWK.",
    ".KWWWWWWWWWWWWK.", ".KLLLLLLLLLLLLK.", ".KLGGGGGGGGGGGK.", ".KLLKKKKKKKKLLK.",
    ".KLLKLLLLLLKLLK.", ".KLLKKKKKKKKLLK.", ".KKKKKKKKKKKKKK.", "................",
};

static const char *const sprite_term[16] = {
    "................", ".KKKKKKKKKKKKKK.", ".KLLLLLLLLLLLLK.", ".KLKKKKKKKKKKLK.",
    ".KLKTTTTTTTTKLK.", ".KLKTYTTTTTTKLK.", ".KLKTTYTYYYTKLK.", ".KLKTYTTTTTTKLK.",
    ".KLKTTTTTTTTKLK.", ".KLKTTTTTTTTKLK.", ".KLKKKKKKKKKKLK.", ".KLLLLLLLLLLLLK.",
    "..KKKKKKKKKKKK..", "...KLLLLLLLLK...", "..KKKKKKKKKKKK..", "................",
};

static const char *const sprite_pad[16] = {
    "................", "..KKKKKKKKKK....", "..KWWWWWWWWKK...", "..KWBBBBBBKWK...",
    "..KWWWWWWWWWWK..", "..KWCCCCCCCCWK..", "..KWWWWWWWWWWK..", "..KWBBBBBBBBWK..",
    "..KWWWWWWWWWWK..", "..KWCCCCCCYYWK..", "..KWWWWWWYYYKW..", "..KWCCCCYYYKWW..",
    "..KWWWWYYYKWWK..", "..KWWWWWWWWWWK..", "..KKKKKKKKKKKK..", "................",
};

static const char *const sprite_mac[16] = {
    "................", "..KKKKKKKKKKKK..", ".KLLLLLLLLLLLLK.", ".KLKKKKKKKKKKLK.",
    ".KLKCCCCCCCCKLK.", ".KLKCKKCCKKCKLK.", ".KLKCCCCCCCCKLK.", ".KLKCKCCCCKCKLK.",
    ".KLKCCKKKKCCKLK.", ".KLKCCCCCCCCKLK.", ".KLKKKKKKKKKKLK.", ".KLLLLLLLLLLLLK.",
    ".KLLLLKKKKKKLLK.", ".KGGGGGGGGGGGGK.", "..KKKKKKKKKKKK..", "................",
};

static const char *const sprite_cpp[16] = {
    "................", "......KKKK......", "....KKYYYYKK....", "...KYWWWWWWYK...",
    "..KYWWKKKKWWYK..", ".KYWWKWWWWWWWYK.", ".KYWWKWWBBBBWYK.", ".KYWWKWWWWWWWYK.",
    ".KYWWKWWBBBBWYK.", ".KYWWKWWWWWWWYK.", "..KYWWKKKKWWYK..", "...KYWWWWWWYK...",
    "....KKYYYYKK....", "......KKKK......", "................", "................",
};

static const char *const sprite_trash[16] = {
    "................", ".....KKKKKK.....", "..KKKKKKKKKKKK..", ".KWWWWWWWWWWWWK.",
    ".KKKKKKKKKKKKKK.", "..KWLWLWLWLWLK..", "..KWLWLWLWLWLK..", "..KWLWLWLWLWLK..",
    "..KWLWLWLWLWLK..", "..KWLWLWLWLWLK..", "..KWLWLWLWLWLK..", "..KWLWLWLWLWLK..",
    "..KWLWLWLWLWLK..", "..KGGGGGGGGGGK..", "...KKKKKKKKKK...", "................",
};

static uint32_t sprite_color(char ch) {
    switch (ch) {
        case 'K': return MAC_BLACK;
        case 'W': return MAC_WHITE;
        case 'L': return 0x00DCE0E8U;
        case 'G': return 0x008C929EU;
        case 'B': return 0x001D4ED8U;
        case 'C': return 0x0038BDF8U;
        case 'T': return 0x000F172AU;
        case 'Y': return 0x00FACC15U;
        default:  return 0;
    }
}

static void gui_draw_sprite_16x2(int32_t px, int32_t py, const char *const rows[16], bool selected) {
    for (int32_t r = 0; r < 16; r++) {
        for (int32_t c = 0; c < 16; c++) {
            char ch = rows[r][c];
            if (ch == '.') continue;
            uint32_t col = sprite_color(ch);
            if (selected) {
                col = ((col >> 1) & 0x007F7F7FU) | 0x00000040U;
            }
            gui_fill_rect(px + c * 2, py + r * 2, 2, 2, col);
        }
    }
}

struct desktop_icon_def {
    const char *label;
    const char *const *sprite;
    int32_t x, y;
};

static const struct desktop_icon_def desktop_icons[6] = {
    { "FulmenHD",  sprite_hd,    718, 36 },
    { "Terminal",  sprite_term,  718, 120 },
    { "CodePad",   sprite_pad,   718, 204 },
    { "About Mac", sprite_mac,   718, 288 },
    { "Run C/C++", sprite_cpp,   718, 372 },
    { "Trash",     sprite_trash, 718, 468 },
};

static void gui_draw_desktop_icons(void) {
    for (int i = 0; i < 6; i++) {
        const struct desktop_icon_def *ic = &desktop_icons[i];
        bool sel = (selected_icon == i);
        gui_draw_sprite_16x2(ic->x + 18, ic->y, ic->sprite, sel);

        int32_t len = (int32_t)strlen(ic->label);
        int32_t lw = len * 8 + 6;
        int32_t lx = ic->x + (68 - lw) / 2;
        int32_t ly = ic->y + 36;
        uint32_t lbg = sel ? MAC_NAVY : themes[active_theme].label_bg;
        gui_fill_rect(lx, ly, lw, 16, lbg);
        if (sel) gui_outline(lx, ly, lw, 16, MAC_WHITE);
        gui_draw_text_fit(lx + 3, ly, len, ic->label, MAC_WHITE, lbg, false);
    }
}

/* -------------------------------------------------------------------------- */
/*  Window Z-Order, Chrome & Geometry Helpers                                 */
/* -------------------------------------------------------------------------- */

static void gui_focus_window(int wid) {
    int pos = -1;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (z_order[i] == wid) { pos = i; break; }
    }
    if (pos >= 0) {
        for (int i = pos; i < WIN_COUNT - 1; i++) {
            z_order[i] = z_order[i + 1];
        }
        z_order[WIN_COUNT - 1] = wid;
    }
    wins[wid].visible = true;
}

static int gui_top_visible_window(void) {
    for (int i = WIN_COUNT - 1; i >= 0; i--) {
        if (wins[z_order[i]].visible) return z_order[i];
    }
    return -1;
}

static void gui_toggle_zoom(int wid) {
    struct gui_win *w = &wins[wid];
    if (!w->zoomed) {
        w->norm_x = w->x;
        w->norm_y = w->y;
        w->norm_w = w->w;
        w->norm_h = w->h;
        w->x = 8;
        w->y = 28;
        w->w = 694;
        w->h = 562;
        w->zoomed = true;
    } else {
        w->x = w->norm_x;
        w->y = w->norm_y;
        w->w = w->norm_w;
        w->h = w->norm_h;
        w->zoomed = false;
    }
}

static void gui_reset_layout(void) {
    wins[WIN_ABOUT] = (struct gui_win){
        WIN_ABOUT, "About This Macintosh",
        10, 32, 306, 224, 10, 32, 306, 224, true, false
    };
    wins[WIN_FINDER] = (struct gui_win){
        WIN_FINDER, "FulmenHD",
        10, 266, 306, 320, 10, 266, 306, 320, true, false
    };
    wins[WIN_EDITOR] = (struct gui_win){
        WIN_EDITOR, "SimpleText - CodePad",
        324, 32, 376, 294, 324, 32, 376, 294, true, false
    };
    wins[WIN_TERM] = (struct gui_win){
        WIN_TERM, "MPW Shell - Terminal",
        324, 336, 376, 250, 324, 336, 376, 250, true, false
    };
    z_order[0] = WIN_ABOUT;
    z_order[1] = WIN_FINDER;
    z_order[2] = WIN_EDITOR;
    z_order[3] = WIN_TERM;
}

static void copy_trunc(char *dst, size_t max_chars, const char *src) {
    size_t len = src ? strlen(src) : 0U;
    if (len <= max_chars) {
        if (len > 0U) memcpy(dst, src, len);
        dst[len] = '\0';
        return;
    }
    if (max_chars > 3U) {
        memcpy(dst, src, max_chars - 3U);
        memcpy(dst + max_chars - 3U, "...", 4U);
    } else {
        dst[0] = '\0';
    }
}

static void gui_draw_window_chrome(const struct gui_win *w, bool is_active) {
    /* 2px classic Mac drop shadow */
    gui_fill_rect(w->x + 2, w->y + w->h, w->w, 2, MAC_BLACK);
    gui_fill_rect(w->x + w->w, w->y + 2, 2, w->h, MAC_BLACK);

    /* Outer Platinum window body & 3D bevel */
    gui_draw_bevel_box(w->x, w->y, w->w, w->h, MAC_WIN_BG, false);

    /* Titlebar */
    uint32_t tb_bg = is_active ? MAC_PLAT_LIGHT : MAC_WIN_BG;
    gui_fill_rect(w->x + 2, w->y + 2, w->w - 4, GUI_TITLEBAR_H - 2, tb_bg);
    gui_fill_rect(w->x + 1, w->y + GUI_TITLEBAR_H, w->w - 2, 1, MAC_BLACK);

    if (is_active) {
        /* 6 classic horizontal Mac pinstripes */
        for (int32_t p = 4; p <= 14; p += 2) {
            gui_fill_rect(w->x + 4, w->y + p, w->w - 8, 1, MAC_PLAT_STRIPE);
        }
        /* Close box [x] & Zoom box [+] */
        gui_fill_rect(w->x + 6, w->y + 3, 14, 14, tb_bg);
        gui_fill_rect(w->x + 7, w->y + 4, 12, 12, MAC_WHITE);
        gui_outline(w->x + 7, w->y + 4, 12, 12, MAC_BLACK);
        gui_fill_rect(w->x + 10, w->y + 9, 6, 2, MAC_PLAT_DARK);

        gui_fill_rect(w->x + w->w - 20, w->y + 3, 14, 14, tb_bg);
        gui_fill_rect(w->x + w->w - 19, w->y + 4, 12, 12, MAC_WHITE);
        gui_outline(w->x + w->w - 19, w->y + 4, 12, 12, MAC_BLACK);
        gui_outline(w->x + w->w - 19, w->y + 4, 7, 7, MAC_BLACK);
    }

    /* Centered title with clean Platinum backing behind text */
    char title_buf[64];
    if (w->id == WIN_EDITOR) {
        char sp[58];
        copy_trunc(sp, 56U, ed_path);
        snprintf(title_buf, sizeof(title_buf), "%s%s", sp, ed_dirty ? "*" : "");
    } else {
        copy_trunc(title_buf, 60U, w->title);
    }
    int32_t tlen = (int32_t)strlen(title_buf);
    int32_t max_chars = (w->w - 56) / 8;
    if (max_chars < 4) max_chars = 4;
    if (tlen > max_chars) tlen = max_chars;
    int32_t tx = w->x + (w->w - tlen * 8) / 2;
    gui_fill_rect(tx - 6, w->y + 2, tlen * 8 + 12, 17, tb_bg);
    gui_draw_text_fit(tx, w->y + 2, tlen, title_buf,
                      is_active ? MAC_BLACK : MAC_PLAT_DARK, tb_bg, false);
}

/* -------------------------------------------------------------------------- */
/*  Shared Terminal Command Execution Helper                                  */
/* -------------------------------------------------------------------------- */

static void gui_append_term(const char *msg) {
    if (!msg) return;
    while (*msg) {
        if (gui_term_len + 1U >= GUI_TERM_CAP) {
            size_t drop = GUI_TERM_CAP / 2U;
            while (drop < gui_term_len && gui_term_buf[drop] != '\n') drop++;
            if (drop < gui_term_len && gui_term_buf[drop] == '\n') drop++;
            if (drop == 0 || drop >= gui_term_len) drop = gui_term_len / 2U;
            size_t rem = gui_term_len - drop;
            memmove(gui_term_buf, gui_term_buf + drop, rem);
            gui_term_len = rem;
        }
        gui_term_buf[gui_term_len++] = *msg++;
        gui_term_buf[gui_term_len] = '\0';
    }
}

static void gui_record_cmd_hist(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (cmd_hist_count > 0 && strcmp(cmd_hist[cmd_hist_count - 1U], cmd) == 0) {
        cmd_hist_pos = -1;
        return;
    }
    if (cmd_hist_count >= GUI_HIST_MAX) {
        for (size_t i = 1; i < GUI_HIST_MAX; i++) {
            strcpy(cmd_hist[i - 1U], cmd_hist[i]);
        }
        cmd_hist_count = GUI_HIST_MAX - 1U;
    }
    strncpy(cmd_hist[cmd_hist_count], cmd, GUI_CMD_MAX - 1U);
    cmd_hist[cmd_hist_count][GUI_CMD_MAX - 1U] = '\0';
    cmd_hist_count++;
    cmd_hist_pos = -1;
}

static void gui_set_display_mode(bool gui_on) {
    if (gui_on) {
        console_set_fb_mirror(false);
        fb_set_gui_mode(true);
        fb_set_render_target(backbuf);
    } else {
        console_set_capture(NULL, 0, NULL);
        fb_set_render_target(NULL);
        fb_set_gui_mode(false);
        console_set_fb_mirror(true);
        fb_clear();
    }
}

static bool node_is_elf(const struct vfs_node *n) {
    return n && n->size >= 4U && n->data && n->data[0] == 0x7FU && n->data[1] == 'E';
}

static bool node_is_c_exe(const struct vfs_node *n) {
    return n && n->size > 10U && n->data && strncmp((const char *)n->data, "#!fulmen-c", 10) == 0;
}

static void finder_delete_selected(bool from_trash) {
    if (finder_sel_path[0] && strncmp(finder_sel_path, "/bin/", 5) != 0) {
        if (vfs_remove(finder_sel_path, false) == 0) {
            char sp[45];
            copy_trunc(sp, 44U, finder_sel_path);
            snprintf(ed_status, sizeof(ed_status), "Deleted %s", sp);
            if (from_trash) gui_append_term("[Trash] Moved selected file to Trash.\n");
        }
    } else if (from_trash) {
        gui_append_term("[Trash] Select a /home or /tmp file in FulmenHD, then click Trash.\n");
    }
}

static void gui_run_shell_cmd(const char *cmd, bool focus_terminal) {
    if (!cmd || !*cmd) return;

    if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        gui_term_len = 0;
        gui_term_buf[0] = '\0';
        return;
    }
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        gui_append_term("[Terminal] Session closed. Use menus to shut down or launch apps.\n");
        return;
    }
    if (strcmp(cmd, "gui") == 0 || strcmp(cmd, "desktop") == 0) {
        gui_append_term("[GUI] Classic FulmenOS Desktop is already active.\n");
        return;
    }

    char hdr[160];
    snprintf(hdr, sizeof(hdr), "$ %s\n", cmd);
    gui_append_term(hdr);

    bool is_fullscreen_tui =
        (strncmp(cmd, "nano", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) ||
        (strncmp(cmd, "edit", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' '));

    if (is_fullscreen_tui) {
        gui_set_display_mode(false);
        shell_exec_line(cmd);
        gui_set_display_mode(true);
        return;
    }

    console_set_capture(gui_term_buf, GUI_TERM_CAP, &gui_term_len);
    shell_exec_line(cmd);
    console_set_capture(NULL, 0, NULL);

    if (focus_terminal) {
        gui_focus_window(WIN_TERM);
    }
}

/* -------------------------------------------------------------------------- */
/*  SimpleText / CodePad Editor Helpers & Rendering                           */
/* -------------------------------------------------------------------------- */

static bool is_cpp_filename(const char *path) {
    size_t len = strlen(path);
    return (len >= 4 && (strcmp(path + len - 4, ".cpp") == 0 || strcmp(path + len - 4, ".cxx") == 0)) ||
           (len >= 3 && strcmp(path + len - 3, ".cc") == 0);
}

static void editor_set_content(const char *path, const char *text) {
    strncpy(ed_path, path, sizeof(ed_path) - 1U);
    ed_path[sizeof(ed_path) - 1U] = '\0';
    size_t n = text ? strlen(text) : 0;
    if (n >= GUI_ED_CAP) n = GUI_ED_CAP - 1U;
    if (n > 0) memcpy(ed_buf, text, n);
    ed_buf[n] = '\0';
    ed_len = n;
    ed_cursor = 0;
    ed_scroll_row = 0;
    ed_dirty = false;
}

static void editor_load_default(void) {
    struct vfs_node *n = vfs_resolve("/home/hello.c");
    if (n && n->type == VFS_FILE && n->data) {
        editor_set_content("/home/hello.c", (const char *)n->data);
        return;
    }
    const char *starter =
        "#include <stdio.h>\n"
        "\n"
        "int main() {\n"
        "  for (int i = 1; i <= 5; i++) {\n"
        "    printf(\"Mac %d^2 = %d\\n\", i, i * i);\n"
        "  }\n"
        "  return 0;\n"
        "}\n";
    editor_set_content("/home/hello.c", starter);
    vfs_write(ed_path, ed_buf, ed_len, false);
    ed_dirty = false;
}

static void editor_load_file(const char *path) {
    struct vfs_node *n = vfs_resolve(path);
    if (!n || n->type != VFS_FILE) return;
    if (node_is_elf(n)) {
        gui_append_term("[CodePad] Cannot edit binary ELF file; use Terminal or [Run/Exec].\n");
        return;
    }
    if (node_is_c_exe(n) && n->size > 11U) {
        editor_set_content(path, (const char *)(n->data + 11));
    } else {
        editor_set_content(path, n->data ? (const char *)n->data : "");
    }
    char sp[36];
    copy_trunc(sp, 35U, path);
    snprintf(ed_status, sizeof(ed_status), "Loaded %s (%lu B)", sp, (uint64_t)ed_len);
    gui_focus_window(WIN_EDITOR);
}

static void editor_save(void) {
    int rc = vfs_write(ed_path, ed_buf, ed_len, false);
    if (rc == 0) {
        char sp[36];
        copy_trunc(sp, 35U, ed_path);
        ed_dirty = false;
        snprintf(ed_status, sizeof(ed_status), "Saved %s (%lu B)", sp, (uint64_t)ed_len);
    } else {
        snprintf(ed_status, sizeof(ed_status), "Save failed (err=%d)", rc);
    }
}

static void editor_compile_and_run(void) {
    editor_save();
    char cmd[VFS_PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "%s %s", is_cpp_filename(ed_path) ? "g++" : "gcc", ed_path);
    gui_run_shell_cmd(cmd, true);
}

static void editor_cursor_row_col(int *out_row, int *out_col) {
    int r = 0, c = 0;
    for (size_t i = 0; i < ed_cursor && i < ed_len; i++) {
        if (ed_buf[i] == '\n') { r++; c = 0; }
        else c++;
    }
    *out_row = r;
    *out_col = c;
}

static size_t editor_pos_from_row_col(int target_row, int target_col) {
    int r = 0, c = 0;
    size_t i = 0;
    while (i < ed_len) {
        if (r == target_row) {
            if (ed_buf[i] == '\n' || c >= target_col) return i;
            c++;
        } else if (ed_buf[i] == '\n') {
            r++;
        }
        i++;
    }
    return ed_len;
}

static void editor_ensure_visible(int vis_rows) {
    int r = 0, c = 0;
    editor_cursor_row_col(&r, &c);
    if (r < ed_scroll_row) ed_scroll_row = r;
    if (vis_rows > 0 && r >= ed_scroll_row + vis_rows) {
        ed_scroll_row = r - vis_rows + 1;
    }
}

static void editor_insert_char(char ch, int vis_rows) {
    if (ed_len + 1U >= GUI_ED_CAP) return;
    memmove(ed_buf + ed_cursor + 1U, ed_buf + ed_cursor, ed_len - ed_cursor + 1U);
    ed_buf[ed_cursor++] = ch;
    ed_len++;
    ed_dirty = true;
    editor_ensure_visible(vis_rows);
}

static void editor_backspace(int vis_rows) {
    if (ed_cursor == 0) return;
    memmove(ed_buf + ed_cursor - 1U, ed_buf + ed_cursor, ed_len - ed_cursor + 1U);
    ed_cursor--;
    ed_len--;
    ed_dirty = true;
    editor_ensure_visible(vis_rows);
}

static void editor_delete_at(void) {
    if (ed_cursor >= ed_len) return;
    memmove(ed_buf + ed_cursor, ed_buf + ed_cursor + 1U, ed_len - ed_cursor);
    ed_len--;
    ed_dirty = true;
}

/* -------------------------------------------------------------------------- */
/*  Window Content Drawing Routines                                           */
/* -------------------------------------------------------------------------- */

static const char *task_state_str(enum task_state st) {
    static const char *const names[] = { "NONE", "READY", "RUN", "SLEEP", "BLOCK", "ZOMB" };
    return ((unsigned)st < 6U) ? names[st] : "NONE";
}

static void draw_win_about(const struct gui_win *w) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;

    /* Classic Mac header strip */
    gui_draw_bevel_box(cx, cy, cw, 30, MAC_WHITE, true);
    gui_draw_text_fit(cx + 8, cy + 7, (cw - 16) / 8,
                      "FulmenOS 1.0  (System 8 Platinum)", MAC_NAVY, MAC_WHITE, false);

    struct pmm_stats ps;
    struct heap_stats hs;
    pmm_get_stats(&ps);
    heap_get_stats(&hs);

    char line[72];
    uint64_t ram_used_kb = (ps.used_pages * PAGE_SIZE) / 1024UL;
    uint64_t ram_tot_kb  = (ps.total_pages * PAGE_SIZE) / 1024UL;
    snprintf(line, sizeof(line), "RAM : %5lu / %6lu KB | MMU:%s",
             ram_used_kb, ram_tot_kb, mmu_is_enabled() ? "ON" : "OFF");
    gui_draw_text_fit(cx, cy + 36, cw / 8, line, MAC_BLACK, MAC_WIN_BG, true);
    gui_draw_progress(cx, cy + 53, cw, 11, ps.used_pages, ps.total_pages, MAC_NAVY);

    snprintf(line, sizeof(line), "Heap: %5lu / %6lu KB (%lu blks)",
             hs.used_bytes / 1024UL, hs.total_bytes / 1024UL, (uint64_t)hs.block_count);
    gui_draw_text_fit(cx, cy + 68, cw / 8, line, MAC_BLACK, MAC_WIN_BG, true);
    gui_draw_progress(cx, cy + 85, cw, 11, hs.used_bytes, hs.total_bytes, 0x00108860U);

    /* Scheduler Radio Buttons + Test Button */
    enum sched_policy pol = sched_get_policy();
    int32_t ry = cy + 102;
    gui_draw_radio(cx,       ry, "MLFQ", pol == SCHED_MLFQ);
    gui_draw_radio(cx + 68,  ry, "RR",   pol == SCHED_RR);
    gui_draw_radio(cx + 124, ry, "PRIO", pol == SCHED_PRIO);
    gui_draw_mac_button(cx + cw - 92, ry - 2, 92, 20, "Test Sched", false);

    /* Compact process box */
    int32_t py = cy + 124;
    int32_t ph = w->y + w->h - 8 - py;
    if (ph < 38) return;
    gui_draw_bevel_box(cx, py, cw, ph, MAC_WHITE, true);
    gui_draw_text_fit(cx + 6, py + 3, (cw - 12) / 8,
                      "PID STATE PRI TICKS  NAME", MAC_PLAT_DARK, MAC_WHITE, true);

    size_t max_t = 0;
    struct task_struct *t = sched_get_task_table(&max_t);
    int32_t max_rows = (ph - 22) / 16;
    int32_t drawn = 0;
    for (size_t i = 0; i < max_t && drawn < max_rows; i++) {
        if (t[i].state == TASK_UNUSED) continue;
        snprintf(line, sizeof(line), "%-3d %-5s P%-2u %-6lu %-12s",
                 t[i].pid, task_state_str(t[i].state),
                 t[i].priority, t[i].total_ticks, t[i].name);
        gui_draw_text_fit(cx + 6, py + 20 + drawn * 16, (cw - 12) / 8,
                          line, MAC_BLACK, MAC_WHITE, true);
        drawn++;
    }
}

static size_t finder_collect_children(struct vfs_node **out, size_t max_out) {
    struct vfs_node *dir = vfs_resolve(finder_dirs[finder_dir_idx]);
    size_t cnt = 0;
    if (!dir || dir->type != VFS_DIR) return 0;
    for (struct vfs_node *c = dir->first_child; c && cnt < max_out; c = c->next_sibling) {
        out[cnt++] = c;
    }
    return cnt;
}

static void draw_win_finder(const struct gui_win *w) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;

    /* Directory folder tabs */
    int32_t bw = (cw - 16) / 5;
    for (size_t i = 0; i < 5; i++) {
        gui_draw_mac_button(cx + (int32_t)i * (bw + 4), cy, bw, 20,
                            finder_dirs[i], i == finder_dir_idx);
    }

    int32_t list_y = cy + 26;
    int32_t btn_zone_h = 52;
    int32_t list_h = (w->y + w->h - 8 - btn_zone_h) - list_y;
    if (list_h < 40) list_h = 40;
    gui_draw_bevel_box(cx, list_y, cw, list_h, MAC_WHITE, true);

    struct vfs_node *nodes[32];
    size_t count = finder_collect_children(nodes, 32U);
    if (finder_sel_idx >= (int)count) finder_sel_idx = (count > 0) ? (int)count - 1 : -1;
    if (finder_sel_idx >= 0) {
        snprintf(finder_sel_path, sizeof(finder_sel_path), "%s/%s",
                 finder_dirs[finder_dir_idx], nodes[finder_sel_idx]->name);
    } else {
        finder_sel_path[0] = '\0';
    }

    int32_t vis_rows = (list_h - 6) / 16;
    if (finder_sel_idx >= finder_scroll + vis_rows) finder_scroll = finder_sel_idx - vis_rows + 1;
    if (finder_sel_idx >= 0 && finder_sel_idx < finder_scroll) finder_scroll = finder_sel_idx;
    if (finder_scroll < 0) finder_scroll = 0;

    char row[72];
    for (int32_t r = 0; r < vis_rows; r++) {
        int idx = finder_scroll + r;
        int32_t ry = list_y + 3 + r * 16;
        if (idx < (int)count) {
            struct vfs_node *n = nodes[idx];
            const char *tag = "[TXT]";
            if (n->type == VFS_DIR) tag = "[DIR]";
            else if (node_is_elf(n)) tag = "[ELF]";
            else if (node_is_c_exe(n)) tag = "[EXE]";
            else if (is_cpp_filename(n->name)) tag = "[C++]";
            else if (strstr(n->name, ".c") != NULL) tag = "[ C ]";

            bool sel = (idx == finder_sel_idx);
            uint32_t fg = sel ? MAC_WHITE : MAC_BLACK;
            uint32_t bg = sel ? MAC_NAVY : MAC_WHITE;
            char short_name[19];
            copy_trunc(short_name, 18U, n->name);
            gui_fill_rect(cx + 3, ry, cw - 6, 16, bg);
            snprintf(row, sizeof(row), "%s %-18s %6lu B", tag, short_name, (uint64_t)n->size);
            gui_draw_text_fit(cx + 6, ry, (cw - 12) / 8, row, fg, bg, true);
        } else if (count == 0 && r == 0) {
            gui_draw_text_fit(cx + 6, ry, (cw - 12) / 8, "(empty folder)",
                              MAC_PLAT_DARK, MAC_WHITE, true);
        }
    }

    /* Action buttons at bottom */
    int32_t b1y = list_y + list_h + 6;
    int32_t b2y = b1y + 24;
    int32_t w3 = (cw - 8) / 3;
    gui_draw_mac_button(cx,                b1y, w3, 20, "Open/Edit", false);
    gui_draw_mac_button(cx + w3 + 4,       b1y, w3, 20, "Run/Exec",  false);
    gui_draw_mac_button(cx + (w3 + 4) * 2, b1y, cw - (w3 + 4) * 2, 20, "Nano", false);

    gui_draw_mac_button(cx,                b2y, w3, 20, "+New .c",   false);
    gui_draw_mac_button(cx + w3 + 4,       b2y, w3, 20, "+New .cpp", false);
    gui_draw_mac_button(cx + (w3 + 4) * 2, b2y, cw - (w3 + 4) * 2, 20, "Delete", false);
}

static void draw_win_editor(const struct gui_win *w) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;

    /* Toolbar buttons */
    int32_t bw = (cw - 12) / 4;
    gui_draw_mac_button(cx,                cy, bw, 20, "Save ^S",  false);
    gui_draw_mac_button(cx + bw + 4,       cy, bw, 20, "Run ^R",   false);
    gui_draw_mac_button(cx + (bw + 4) * 2, cy, bw, 20, "Clear",    false);
    gui_draw_mac_button(cx + (bw + 4) * 3, cy, cw - (bw + 4) * 3, 20, "Nano", false);

    /* Text paper area with line numbers */
    int32_t py = cy + 25;
    int32_t ph = (w->y + w->h - 24) - py;
    if (ph < 32) ph = 32;
    gui_draw_bevel_box(cx, py, cw, ph, MAC_PAPER_BG, true);
    gui_fill_rect(cx + 2, py + 2, 28, ph - 4, MAC_GUTTER_BG);
    gui_fill_rect(cx + 30, py + 2, 1, ph - 4, MAC_PLAT_MID);

    int32_t vis_rows = (ph - 6) / 16;
    int32_t vis_cols = (cw - 36) / 8;
    if (vis_cols < 8) vis_cols = 8;

    int cur_r = 0, cur_c = 0;
    editor_cursor_row_col(&cur_r, &cur_c);

    size_t pos = 0;
    int row_idx = 0;
    while (row_idx < ed_scroll_row && pos < ed_len) {
        if (ed_buf[pos++] == '\n') row_idx++;
    }

    int32_t h_off = 0;
    if (cur_c >= vis_cols) h_off = cur_c - vis_cols + 1;

    char lnum[8];
    for (int32_t vr = 0; vr < vis_rows; vr++) {
        int32_t ry = py + 3 + vr * 16;
        int actual_row = ed_scroll_row + vr;
        if (vr == 0 || actual_row <= cur_r || pos < ed_len) {
            bool has_line = (pos < ed_len) || (vr == 0) ||
                            (ed_len > 0 && ed_buf[ed_len - 1] == '\n' && actual_row == cur_r);
            if (has_line) {
                snprintf(lnum, sizeof(lnum), "%3d", actual_row + 1);
                gui_draw_text_fit(cx + 4, ry, 3, lnum, MAC_PLAT_DARK, MAC_GUTTER_BG, false);
            }
        }

        size_t line_start = pos;
        int line_chars = 0;
        while (pos < ed_len && ed_buf[pos] != '\n') {
            pos++;
            line_chars++;
        }
        bool ended_nl = (pos < ed_len && ed_buf[pos] == '\n');

        for (int32_t vc = 0; vc < vis_cols; vc++) {
            int col_idx = h_off + vc;
            char ch = (col_idx < line_chars) ? ed_buf[line_start + (size_t)col_idx] : ' ';
            bool is_cur = (actual_row == cur_r && col_idx == cur_c);
            uint32_t fg = is_cur ? MAC_WHITE : MAC_BLACK;
            uint32_t bg = is_cur ? MAC_NAVY : MAC_PAPER_BG;
            fb_draw_char_px((uint32_t)(cx + 34 + vc * 8), (uint32_t)ry, ch, fg, bg);
        }

        if (!ended_nl) break;
        pos++;
    }

    /* Status footer */
    gui_draw_text_fit(cx, w->y + w->h - 20, cw / 8, ed_status, MAC_BLACK, MAC_WIN_BG, true);
}

static void draw_win_term(const struct gui_win *w, bool is_active) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;

    /* Quick-action toolbar */
    static const char *const qlabels[6] = { "Clear", "ls -l", "ps", "gcc", "calc", "el0" };
    int32_t bw = (cw - 20) / 6;
    for (int i = 0; i < 6; i++) {
        gui_draw_mac_button(cx + i * (bw + 4), cy, bw, 20, qlabels[i], false);
    }

    /* Terminal output box */
    int32_t ty = cy + 25;
    int32_t th = (w->y + w->h - 32) - ty;
    if (th < 32) th = 32;
    gui_draw_bevel_box(cx, ty, cw, th, MAC_TERM_BG, true);

    int32_t cols = (cw - 12) / 8;
    int32_t rows = (th - 6) / 16;
    if (cols < 10) cols = 10;
    if (rows < 1) rows = 1;

    static size_t lstart[160];
    static size_t llen[160];
    size_t lcnt = 0;
    size_t pos = 0;
    while (pos <= gui_term_len && lcnt < 160U) {
        size_t st = pos;
        int32_t len = 0;
        while (pos < gui_term_len && gui_term_buf[pos] != '\n' && len < cols) {
            pos++;
            len++;
        }
        lstart[lcnt] = st;
        llen[lcnt] = (size_t)len;
        lcnt++;
        if (pos >= gui_term_len) break;
        if (gui_term_buf[pos] == '\n') {
            pos++;
            if (pos == gui_term_len) break;
        }
    }

    size_t first = (lcnt > (size_t)rows) ? (lcnt - (size_t)rows) : 0U;
    char rbuf[128];
    for (int32_t r = 0; r < rows; r++) {
        size_t idx = first + (size_t)r;
        int32_t ry = ty + 3 + r * 16;
        if (idx < lcnt) {
            size_t len = llen[idx];
            if (len >= sizeof(rbuf)) len = sizeof(rbuf) - 1U;
            memcpy(rbuf, gui_term_buf + lstart[idx], len);
            rbuf[len] = '\0';
            uint32_t fg = (strstr(rbuf, "[PASS]") || strstr(rbuf, "[gcc]") || strstr(rbuf, "[g++]")) ? MAC_TERM_GREEN
                        : (strstr(rbuf, "Error") || strstr(rbuf, "error:") || strstr(rbuf, "FAIL")) ? MAC_TERM_RED
                        : (rbuf[0] == '$') ? MAC_TERM_CYAN
                        : (strstr(rbuf, "[Mini-C]")) ? MAC_TERM_AMBER
                        : MAC_TERM_FG;
            gui_draw_text_fit(cx + 6, ry, cols, rbuf, fg, MAC_TERM_BG, true);
        } else {
            gui_draw_text_fit(cx + 6, ry, cols, "", MAC_TERM_FG, MAC_TERM_BG, true);
        }
    }

    /* Command input line */
    int32_t iy = w->y + w->h - 28;
    gui_draw_bevel_box(cx, iy, cw, 22, MAC_WHITE, true);
    char prompt[GUI_CMD_MAX + 8U];
    int32_t pcols = (cw - 10) / 8;
    size_t show_off = 0;
    if ((int32_t)cmd_len + 4 > pcols && pcols > 6) {
        show_off = cmd_len + 4U - (size_t)pcols;
    }
    snprintf(prompt, sizeof(prompt), "# %s%s", cmd_buf + show_off, is_active ? "_" : "");
    gui_draw_text_fit(cx + 5, iy + 3, pcols, prompt, MAC_BLACK, MAC_WHITE, true);
}

/* -------------------------------------------------------------------------- */
/*  Top Classic Menu Bar & Pull-Down Menus                                    */
/* -------------------------------------------------------------------------- */

struct menu_def {
    const char *title;
    int32_t x, w;
    int item_count;
    const char *items[6];
};

static const struct menu_def mac_menus[5] = {
    { "[*]",     6,   34, 4, { "About FulmenOS...", "Run EL0 Trap Test", "Run Deadlock Test", "Shut Down     ^Q" } },
    { "File",    42,  48, 6, { "New C Program", "New C++ Program", "Open Finder", "Open CodePad", "Save CodePad  ^S", "Delete Selected" } },
    { "Edit",    92,  48, 4, { "Insert C Demo", "Insert C++ Demo", "Clear CodePad", "Clear Terminal" } },
    { "View",    142, 48, 5, { "Show All Windows", "Reset Layout", "Theme: Mac Blue", "Theme: Sys7 Gray", "Theme: Bondi Teal" } },
    { "Special", 192, 70, 5, { "Compile & Run ^R", "Sched: MLFQ", "Sched: Round-Robin", "Sched: Priority", "Shut Down OS" } },
};

static void gui_trigger_menu_item(int m, int item) {
    if (m == 0) { /* Apple menu */
        if (item == 0) gui_focus_window(WIN_ABOUT);
        else if (item == 1) gui_run_shell_cmd("el0_test", true);
        else if (item == 2) gui_run_shell_cmd("deadlock_test", true);
        else if (item == 3) { gui_run_shell_cmd("poweroff", false); }
    } else if (m == 1) { /* File */
        if (item == 0 || item == 1) {
            bool is_cpp = (item == 1);
            char path[64];
            snprintf(path, sizeof(path), "/home/app%d.%s", new_file_seq++, is_cpp ? "cpp" : "c");
            const char *tpl = is_cpp
                ? "#include <iostream>\nusing namespace std;\nint main() {\n    cout << \"Hello Mac C++!\" << endl;\n    return 0;\n}\n"
                : "#include <stdio.h>\nint main() {\n    printf(\"Hello Mac C!\\n\");\n    return 0;\n}\n";
            editor_set_content(path, tpl);
            editor_save();
            finder_dir_idx = 0;
            gui_focus_window(WIN_EDITOR);
        } else if (item == 2) {
            gui_focus_window(WIN_FINDER);
        } else if (item == 3) {
            gui_focus_window(WIN_EDITOR);
        } else if (item == 4) {
            editor_save();
        } else if (item == 5) {
            finder_delete_selected(false);
        }
    } else if (m == 2) { /* Edit */
        if (item == 0) {
            editor_set_content("/home/demo_edit.c",
                "#include <stdio.h>\nstruct Pt { int x; int y; };\nint main() {\n    struct Pt p; p.x = 6; p.y = 7;\n    printf(\"Pt(%d,%d) prod=%d\\n\", p.x, p.y, p.x * p.y);\n    return 0;\n}\n");
            ed_dirty = true;
            gui_focus_window(WIN_EDITOR);
        } else if (item == 1) {
            editor_set_content("/home/demo_edit.cpp",
                "#include <iostream>\nusing namespace std;\nclass Box {\npublic:\n    int v;\n    Box(int n) { v = n; }\n};\nint main() {\n    Box b(42);\n    cout << \"Box=\" << b.v << endl;\n    return 0;\n}\n");
            ed_dirty = true;
            gui_focus_window(WIN_EDITOR);
        } else if (item == 2) {
            editor_set_content(ed_path, "");
            ed_dirty = true;
            gui_focus_window(WIN_EDITOR);
        } else if (item == 3) {
            gui_term_len = 0; gui_term_buf[0] = '\0';
        }
    } else if (m == 3) { /* View */
        if (item == 0) {
            for (int i = 0; i < WIN_COUNT; i++) wins[i].visible = true;
        } else if (item == 1) {
            gui_reset_layout();
        } else if (item >= 2 && item <= 4) {
            active_theme = item - 2;
        }
    } else if (m == 4) { /* Special */
        if (item == 0) editor_compile_and_run();
        else if (item == 1) sched_set_policy(SCHED_MLFQ);
        else if (item == 2) sched_set_policy(SCHED_RR);
        else if (item == 3) sched_set_policy(SCHED_PRIO);
        else if (item == 4) gui_run_shell_cmd("poweroff", false);
    }
}

static void gui_draw_menubar(void) {
    gui_fill_rect(0, 0, (int32_t)FB_WIDTH, GUI_MENUBAR_H - 1, MAC_MENUBAR_BG);
    gui_fill_rect(0, GUI_MENUBAR_H - 1, (int32_t)FB_WIDTH, 1, MAC_BLACK);

    for (int i = 0; i < 5; i++) {
        bool open = (active_menu == i);
        uint32_t bg = open ? MAC_NAVY : MAC_MENUBAR_BG;
        uint32_t fg = open ? MAC_WHITE : ((i == 0) ? 0x000050C0U : MAC_BLACK);
        gui_fill_rect(mac_menus[i].x, 1, mac_menus[i].w, GUI_MENUBAR_H - 2, bg);
        int32_t tx = mac_menus[i].x + (mac_menus[i].w - (int32_t)strlen(mac_menus[i].title) * 8) / 2;
        gui_draw_text_fit(tx, 3, 12, mac_menus[i].title, fg, bg, false);
    }

    uint64_t secs = timer_get_ms() / 1000UL;
    char right[48];
    snprintf(right, sizeof(right), "%-4s | %02lu:%02lu",
             sched_policy_name(sched_get_policy()), secs / 60UL, secs % 60UL);
    gui_draw_text_fit(608, 3, 14, right, MAC_BLACK, MAC_MENUBAR_BG, false);

    gui_draw_mac_button(732, 2, 62, 18, "Shut Down", false);
}

static void gui_draw_open_menu_popup(void) {
    if (active_menu < 0 || active_menu >= 5) return;
    const struct menu_def *md = &mac_menus[active_menu];
    int32_t mx = md->x;
    int32_t my = GUI_MENUBAR_H;
    int32_t mw = 172;
    int32_t mh = md->item_count * 20 + 6;

    gui_fill_rect(mx + 2, my + 2, mw, mh, MAC_BLACK);
    gui_fill_rect(mx, my, mw, mh, MAC_WHITE);
    gui_outline(mx, my, mw, mh, MAC_BLACK);

    for (int i = 0; i < md->item_count; i++) {
        int32_t iy = my + 3 + i * 20;
        bool hov = (i == menu_hover_item);
        uint32_t bg = hov ? MAC_NAVY : MAC_WHITE;
        uint32_t fg = hov ? MAC_WHITE : MAC_BLACK;
        gui_fill_rect(mx + 2, iy, mw - 4, 18, bg);
        gui_draw_text_fit(mx + 8, iy + 1, 20, md->items[i], fg, bg, false);
    }
}

static void gui_draw_all(void) {
    gui_draw_desktop_bg();
    gui_draw_desktop_icons();

    int top = gui_top_visible_window();
    for (int z = 0; z < WIN_COUNT; z++) {
        int wid = z_order[z];
        const struct gui_win *w = &wins[wid];
        if (!w->visible) continue;
        bool is_active = (wid == top);
        gui_draw_window_chrome(w, is_active);

        if (wid == WIN_ABOUT)       draw_win_about(w);
        else if (wid == WIN_FINDER) draw_win_finder(w);
        else if (wid == WIN_EDITOR) draw_win_editor(w);
        else if (wid == WIN_TERM)   draw_win_term(w, is_active);
    }

    gui_draw_menubar();
    gui_draw_open_menu_popup();
}

/* -------------------------------------------------------------------------- */
/*  Window Interaction Handlers (Mouse & Keyboard)                            */
/* -------------------------------------------------------------------------- */

static void finder_activate_node(struct vfs_node *n, bool run_mode) {
    if (!n) return;
    char full[VFS_PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", finder_dirs[finder_dir_idx], n->name);

    if (n->type == VFS_DIR) {
        gui_run_shell_cmd(full, false);
        return;
    }

    bool is_bin = node_is_elf(n) || node_is_c_exe(n);
    if (run_mode || is_bin) {
        if (is_bin) {
            gui_run_shell_cmd(full, true);
        } else {
            char cmd[VFS_PATH_MAX + 12];
            snprintf(cmd, sizeof(cmd), "%s %s", is_cpp_filename(full) ? "g++" : "gcc", full);
            gui_run_shell_cmd(cmd, true);
        }
    } else {
        editor_load_file(full);
    }
}

static void handle_about_click(const struct gui_win *w, int32_t mx, int32_t my) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;
    int32_t ry = cy + 102;

    if (pt_in_rect(mx, my, cx, ry - 2, 64, 20))      sched_set_policy(SCHED_MLFQ);
    else if (pt_in_rect(mx, my, cx + 68, ry - 2, 52, 20)) sched_set_policy(SCHED_RR);
    else if (pt_in_rect(mx, my, cx + 124, ry - 2, 60, 20)) sched_set_policy(SCHED_PRIO);
    else if (pt_in_rect(mx, my, cx + cw - 92, ry - 2, 92, 20)) gui_run_shell_cmd("sched_test", true);
}

static void handle_finder_click(const struct gui_win *w, int32_t mx, int32_t my) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;
    int32_t bw = (cw - 16) / 5;

    for (size_t i = 0; i < 5; i++) {
        if (pt_in_rect(mx, my, cx + (int32_t)i * (bw + 4), cy, bw, 20)) {
            finder_dir_idx = i;
            finder_sel_idx = 0;
            finder_scroll = 0;
            return;
        }
    }

    int32_t list_y = cy + 26;
    int32_t btn_zone_h = 52;
    int32_t list_h = (w->y + w->h - 8 - btn_zone_h) - list_y;
    if (list_h < 40) list_h = 40;

    struct vfs_node *nodes[32];
    size_t count = finder_collect_children(nodes, 32U);

    if (pt_in_rect(mx, my, cx, list_y, cw, list_h)) {
        int32_t r = (my - (list_y + 3)) / 16;
        int idx = finder_scroll + r;
        if (r >= 0 && idx >= 0 && idx < (int)count) {
            uint64_t now = timer_get_ms();
            bool dbl = (idx == finder_last_click_idx && (now - finder_last_click_ms) < 700U);
            finder_sel_idx = idx;
            finder_last_click_idx = idx;
            finder_last_click_ms = now;
            if (dbl) finder_activate_node(nodes[idx], false);
        }
        return;
    }

    int32_t b1y = list_y + list_h + 6;
    int32_t b2y = b1y + 24;
    int32_t w3 = (cw - 8) / 3;
    struct vfs_node *sel = (finder_sel_idx >= 0 && finder_sel_idx < (int)count) ? nodes[finder_sel_idx] : NULL;

    if (pt_in_rect(mx, my, cx, b1y, w3, 20)) {
        finder_activate_node(sel, false);
    } else if (pt_in_rect(mx, my, cx + w3 + 4, b1y, w3, 20)) {
        finder_activate_node(sel, true);
    } else if (pt_in_rect(mx, my, cx + (w3 + 4) * 2, b1y, cw - (w3 + 4) * 2, 20)) {
        if (sel && sel->type == VFS_FILE) {
            char cmd[VFS_PATH_MAX + 8];
            snprintf(cmd, sizeof(cmd), "nano %s/%s", finder_dirs[finder_dir_idx], sel->name);
            gui_run_shell_cmd(cmd, false);
        }
    } else if (pt_in_rect(mx, my, cx, b2y, w3, 20)) {
        gui_trigger_menu_item(1, 0); /* New C */
    } else if (pt_in_rect(mx, my, cx + w3 + 4, b2y, w3, 20)) {
        gui_trigger_menu_item(1, 1); /* New C++ */
    } else if (pt_in_rect(mx, my, cx + (w3 + 4) * 2, b2y, cw - (w3 + 4) * 2, 20)) {
        gui_trigger_menu_item(1, 5); /* Delete */
    }
}

static void handle_editor_click(const struct gui_win *w, int32_t mx, int32_t my) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;
    int32_t bw = (cw - 12) / 4;

    if (pt_in_rect(mx, my, cx, cy, bw, 20)) {
        editor_save();
        return;
    }
    if (pt_in_rect(mx, my, cx + bw + 4, cy, bw, 20)) {
        editor_compile_and_run();
        return;
    }
    if (pt_in_rect(mx, my, cx + (bw + 4) * 2, cy, bw, 20)) {
        editor_set_content(ed_path, "");
        ed_dirty = true;
        return;
    }
    if (pt_in_rect(mx, my, cx + (bw + 4) * 3, cy, cw - (bw + 4) * 3, 20)) {
        editor_save();
        char cmd[VFS_PATH_MAX + 8];
        snprintf(cmd, sizeof(cmd), "nano %s", ed_path);
        gui_run_shell_cmd(cmd, false);
        editor_load_file(ed_path);
        return;
    }

    int32_t py = cy + 25;
    int32_t ph = (w->y + w->h - 24) - py;
    if (pt_in_rect(mx, my, cx + 32, py, cw - 34, ph)) {
        int row = ed_scroll_row + (my - (py + 3)) / 16;
        int col = (mx - (cx + 34)) / 8;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        ed_cursor = editor_pos_from_row_col(row, col);
    }
}

static void handle_term_click(const struct gui_win *w, int32_t mx, int32_t my) {
    int32_t cx = w->x + 10;
    int32_t cy = w->y + GUI_TITLEBAR_H + 6;
    int32_t cw = w->w - 20;
    int32_t bw = (cw - 20) / 6;

    static const char *const qcmds[6] = {
        "clear", "ls -l", "ps", "gcc /src/demo.c", "calc", "el0_test"
    };
    for (int i = 0; i < 6; i++) {
        if (pt_in_rect(mx, my, cx + i * (bw + 4), cy, bw, 20)) {
            gui_run_shell_cmd(qcmds[i], true);
            return;
        }
    }
}

static void handle_desktop_icon_click(int icon_idx) {
    selected_icon = icon_idx;
    if (icon_idx == 0) gui_focus_window(WIN_FINDER);
    else if (icon_idx == 1) gui_focus_window(WIN_TERM);
    else if (icon_idx == 2) gui_focus_window(WIN_EDITOR);
    else if (icon_idx == 3) gui_focus_window(WIN_ABOUT);
    else if (icon_idx == 4) editor_compile_and_run();
    else if (icon_idx == 5) finder_delete_selected(true);
}

static void handle_gui_key(int ch, int *dragging_win) {
    int top = gui_top_visible_window();

    /* Function keys F1-F5 (0x81..0x85) open classic Mac pull-down menus */
    if (ch >= 0x81 && ch <= 0x85) {
        int m = ch - 0x81;
        active_menu = (active_menu == m) ? -1 : m;
        menu_hover_item = (active_menu >= 0) ? 0 : -1;
        return;
    }

    /* Global Ctrl shortcuts */
    if (ch == 0x11) { /* Ctrl+Q: Shut Down */
        gui_run_shell_cmd("poweroff", false);
        return;
    }
    if (ch == 0x13) { /* Ctrl+S: Save CodePad */
        editor_save();
        return;
    }
    if (ch == 0x12) { /* Ctrl+R: Compile & Run CodePad */
        editor_compile_and_run();
        return;
    }
    if (ch == 0x17) { /* Ctrl+W: Close active window */
        if (top >= 0) {
            wins[top].visible = false;
            *dragging_win = -1;
        }
        return;
    }

    /* Decode ANSI escape sequence if any */
    int arrow = 0; /* 1=Up, 2=Down, 3=Right, 4=Left, 5=Home, 6=End, 7=Del */
    if (ch == 27) {
        uint64_t t0 = timer_get_ms();
        int seq0 = -1;
        while ((timer_get_ms() - t0) < 25U) {
            seq0 = uart_getc_nonblock();
            if (seq0 >= 0) break;
        }
        if (seq0 < 0 || (seq0 != '[' && seq0 != 'O')) {
            if (active_menu >= 0) {
                active_menu = -1;
                menu_hover_item = -1;
            }
            return;
        }
        uint64_t t1 = timer_get_ms();
        int seq1 = -1;
        while ((timer_get_ms() - t1) < 25U) {
            seq1 = uart_getc_nonblock();
            if (seq1 >= 0) break;
        }
        if (seq1 == 'A') arrow = 1;
        else if (seq1 == 'B') arrow = 2;
        else if (seq1 == 'C') arrow = 3;
        else if (seq1 == 'D') arrow = 4;
        else if (seq1 == 'H') arrow = 5;
        else if (seq1 == 'F') arrow = 6;
        else if (seq1 == '3') {
            (void)uart_getc_nonblock();
            arrow = 7;
        }
    }

    /* Open menu keyboard navigation */
    if (active_menu >= 0) {
        int cnt = mac_menus[active_menu].item_count;
        if (arrow == 1) menu_hover_item = (menu_hover_item + cnt - 1) % cnt;
        else if (arrow == 2) menu_hover_item = (menu_hover_item + 1) % cnt;
        else if (arrow == 4) { active_menu = (active_menu + 4) % 5; menu_hover_item = 0; }
        else if (arrow == 3) { active_menu = (active_menu + 1) % 5; menu_hover_item = 0; }
        else if (ch == '\r' || ch == '\n') {
            int m = active_menu, it = (menu_hover_item >= 0) ? menu_hover_item : 0;
            active_menu = -1; menu_hover_item = -1;
            gui_trigger_menu_item(m, it);
        }
        return;
    }

    if (top == WIN_EDITOR) {
        int ph = (wins[WIN_EDITOR].y + wins[WIN_EDITOR].h - 24) -
                 (wins[WIN_EDITOR].y + GUI_TITLEBAR_H + 31);
        int vis_rows = (ph - 6) / 16;
        if (vis_rows < 1) vis_rows = 1;

        int r = 0, c = 0;
        editor_cursor_row_col(&r, &c);
        if (arrow == 4 && ed_cursor > 0) ed_cursor--;
        else if (arrow == 3 && ed_cursor < ed_len) ed_cursor++;
        else if (arrow == 1 && r > 0) ed_cursor = editor_pos_from_row_col(r - 1, c);
        else if (arrow == 2) ed_cursor = editor_pos_from_row_col(r + 1, c);
        else if (arrow == 5) ed_cursor = editor_pos_from_row_col(r, 0);
        else if (arrow == 6) ed_cursor = editor_pos_from_row_col(r, 9999);
        else if (arrow == 7) editor_delete_at();
        else if (ch == '\b' || ch == 127) editor_backspace(vis_rows);
        else if (ch == '\r' || ch == '\n') editor_insert_char('\n', vis_rows);
        else if (ch == '\t') { editor_insert_char(' ', vis_rows); editor_insert_char(' ', vis_rows); }
        else if (ch >= 0x20 && ch <= 0x7E) editor_insert_char((char)ch, vis_rows);
        editor_ensure_visible(vis_rows);
        return;
    }

    if (top == WIN_FINDER) {
        struct vfs_node *nodes[32];
        size_t cnt = finder_collect_children(nodes, 32U);
        if (arrow == 1 && finder_sel_idx > 0) finder_sel_idx--;
        else if (arrow == 2 && finder_sel_idx + 1 < (int)cnt) finder_sel_idx++;
        else if (ch == '\t') {
            finder_dir_idx = (finder_dir_idx + 1U) % 5U;
            finder_sel_idx = 0;
            finder_scroll = 0;
        } else if ((ch == '\r' || ch == '\n') && finder_sel_idx >= 0 && finder_sel_idx < (int)cnt) {
            finder_activate_node(nodes[finder_sel_idx], false);
        }
        return;
    }

    if (top == WIN_TERM) {
        if (arrow == 1 && cmd_hist_count > 0) {
            if (cmd_hist_pos < 0) cmd_hist_pos = (int)cmd_hist_count - 1;
            else if (cmd_hist_pos > 0) cmd_hist_pos--;
            strcpy(cmd_buf, cmd_hist[cmd_hist_pos]);
            cmd_len = strlen(cmd_buf);
        } else if (arrow == 2 && cmd_hist_count > 0) {
            if (cmd_hist_pos >= 0 && cmd_hist_pos + 1 < (int)cmd_hist_count) {
                cmd_hist_pos++;
                strcpy(cmd_buf, cmd_hist[cmd_hist_pos]);
                cmd_len = strlen(cmd_buf);
            } else {
                cmd_hist_pos = -1;
                cmd_len = 0;
                cmd_buf[0] = '\0';
            }
        } else if (ch == '\r' || ch == '\n') {
            cmd_buf[cmd_len] = '\0';
            if (cmd_len > 0) {
                char copy[GUI_CMD_MAX];
                strcpy(copy, cmd_buf);
                gui_record_cmd_hist(copy);
                cmd_len = 0;
                cmd_buf[0] = '\0';
                gui_run_shell_cmd(copy, true);
            }
        } else if (ch == '\b' || ch == 127) {
            if (cmd_len > 0) cmd_buf[--cmd_len] = '\0';
        } else if (ch >= 0x20 && ch <= 0x7E && cmd_len + 1U < GUI_CMD_MAX) {
            cmd_buf[cmd_len++] = (char)ch;
            cmd_buf[cmd_len] = '\0';
        }
        return;
    }

    /* When About Mac is active, numeric/Tab/Ctrl shortcuts still assist keyboard users */
    if (ch == '\t') {
        int next = (top + 1) % WIN_COUNT;
        gui_focus_window(next);
    }
}

void cmd_gui(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!fb_is_active()) {
        kprintf("gui: Graphical framebuffer (ramfb) is not active in headless mode.\n");
        return;
    }

    kprintf(ANSI_CYAN "[GUI]" ANSI_RESET " Launching FulmenOS Classic Macintosh Desktop...\n");

    backbuf = (uint32_t *)kmalloc(FB_WIDTH * FB_HEIGHT * FB_BPP);

    gui_term_len = 0;
    gui_term_buf[0] = '\0';
    cmd_len = 0;
    cmd_buf[0] = '\0';
    cmd_hist_count = 0;
    cmd_hist_pos = -1;
    active_menu = -1;
    menu_hover_item = -1;
    selected_icon = -1;

    gui_reset_layout();
    editor_load_default();
    gui_append_term("Welcome to FulmenOS Classic Mac Desktop\n");
    gui_append_term("Type commands below, edit C/C++ in CodePad,\n");
    gui_append_term("or use menus/icons. Shut Down: Ctrl+Q\n");

    virtio_mouse_set_pos(400, 300);
    struct mouse_state ms;
    virtio_mouse_get_state(&ms);
    int32_t prev_mx = ms.x;
    int32_t prev_my = ms.y;

    gui_set_display_mode(true);
    gui_running = true;

    int dragging_win = -1;
    int32_t drag_off_x = 0, drag_off_y = 0;
    uint64_t last_refresh = timer_get_ticks();
    bool dirty = true;

    while (gui_running && !shell_is_poweroff_requested()) {
        if (dirty) {
            gui_draw_all();
            if (backbuf) fb_present_with_cursor(backbuf, ms.x, ms.y);
            prev_mx = ms.x;
            prev_my = ms.y;
            dirty = false;
        }

        virtio_mouse_get_state(&ms);

        /* Handle hover in open pull-down menu */
        if (active_menu >= 0) {
            const struct menu_def *md = &mac_menus[active_menu];
            int new_hov = -1;
            if (pt_in_rect(ms.x, ms.y, md->x, GUI_MENUBAR_H + 3, 172, md->item_count * 20)) {
                new_hov = (ms.y - (GUI_MENUBAR_H + 3)) / 20;
            }
            if (new_hov != menu_hover_item) {
                menu_hover_item = new_hov;
                dirty = true;
            }
        }

        /* Handle active window drag */
        if (dragging_win >= 0) {
            if (!ms.left_down || ms.left_released) {
                dragging_win = -1;
            } else if (ms.x != prev_mx || ms.y != prev_my) {
                struct gui_win *dw = &wins[dragging_win];
                int32_t nx = ms.x - drag_off_x;
                int32_t ny = ms.y - drag_off_y;
                if (nx < -(dw->w - 80)) nx = -(dw->w - 80);
                if (nx > (int32_t)FB_WIDTH - 80) nx = (int32_t)FB_WIDTH - 80;
                if (ny < GUI_MENUBAR_H) ny = GUI_MENUBAR_H;
                if (ny > (int32_t)FB_HEIGHT - GUI_TITLEBAR_H) ny = (int32_t)FB_HEIGHT - GUI_TITLEBAR_H;
                if (nx != dw->x || ny != dw->y) {
                    dw->x = nx;
                    dw->y = ny;
                    dirty = true;
                }
            }
        }

        if (ms.left_pressed) {
            /* 1. Top menu bar click */
            if (ms.y < GUI_MENUBAR_H) {
                if (pt_in_rect(ms.x, ms.y, 732, 2, 62, 18)) {
                    gui_run_shell_cmd("poweroff", false);
                } else {
                    int clicked_m = -1;
                    for (int i = 0; i < 5; i++) {
                        if (pt_in_rect(ms.x, ms.y, mac_menus[i].x, 0, mac_menus[i].w, GUI_MENUBAR_H)) {
                            clicked_m = i;
                            break;
                        }
                    }
                    active_menu = (active_menu == clicked_m) ? -1 : clicked_m;
                    menu_hover_item = -1;
                }
                dirty = true;
            }
            /* 2. Open menu popup item click */
            else if (active_menu >= 0) {
                const struct menu_def *md = &mac_menus[active_menu];
                if (pt_in_rect(ms.x, ms.y, md->x, GUI_MENUBAR_H, 172, md->item_count * 20 + 6)) {
                    int item = (ms.y - (GUI_MENUBAR_H + 3)) / 20;
                    int m = active_menu;
                    active_menu = -1;
                    menu_hover_item = -1;
                    if (item >= 0 && item < md->item_count) {
                        gui_trigger_menu_item(m, item);
                    }
                } else {
                    active_menu = -1;
                    menu_hover_item = -1;
                }
                dirty = true;
            }
            /* 3. Top-to-bottom window hit test */
            else {
                int hit_win = -1;
                for (int z = WIN_COUNT - 1; z >= 0; z--) {
                    int wid = z_order[z];
                    const struct gui_win *w = &wins[wid];
                    if (w->visible && pt_in_rect(ms.x, ms.y, w->x, w->y, w->w, w->h)) {
                        hit_win = wid;
                        break;
                    }
                }

                if (hit_win >= 0) {
                    bool was_top = (hit_win == gui_top_visible_window());
                    gui_focus_window(hit_win);
                    struct gui_win *w = &wins[hit_win];

                    if (was_top && pt_in_rect(ms.x, ms.y, w->x + 5, w->y + 3, 16, 15)) {
                        w->visible = false;
                    } else if (was_top && pt_in_rect(ms.x, ms.y, w->x + w->w - 22, w->y + 3, 16, 15)) {
                        gui_toggle_zoom(hit_win);
                    } else if (ms.y < w->y + GUI_TITLEBAR_H) {
                        dragging_win = hit_win;
                        drag_off_x = ms.x - w->x;
                        drag_off_y = ms.y - w->y;
                    } else {
                        if (hit_win == WIN_ABOUT)       handle_about_click(w, ms.x, ms.y);
                        else if (hit_win == WIN_FINDER) handle_finder_click(w, ms.x, ms.y);
                        else if (hit_win == WIN_EDITOR) handle_editor_click(w, ms.x, ms.y);
                        else if (hit_win == WIN_TERM)   handle_term_click(w, ms.x, ms.y);
                    }
                    dirty = true;
                } else {
                    /* 4. Desktop icon click */
                    selected_icon = -1;
                    for (int i = 0; i < 6; i++) {
                        if (pt_in_rect(ms.x, ms.y, desktop_icons[i].x, desktop_icons[i].y, 68, 56)) {
                            handle_desktop_icon_click(i);
                            break;
                        }
                    }
                    dirty = true;
                }
            }
        } else if (!dirty && (ms.x != prev_mx || ms.y != prev_my)) {
            if (backbuf) {
                fb_move_cursor_only(backbuf, prev_mx, prev_my, ms.x, ms.y);
            }
            prev_mx = ms.x;
            prev_my = ms.y;
        }

        int ch = uart_getc_nonblock();
        if (ch >= 0) {
            handle_gui_key(ch, &dragging_win);
            dirty = true;
            last_refresh = timer_get_ticks();
            continue;
        }

        uint64_t now = timer_get_ticks();
        if ((now - last_refresh) >= 100U) {
            last_refresh = now;
            dirty = true;
        }

        if (!dirty) {
            __asm__ volatile("wfi");
        }
    }

    gui_set_display_mode(false);
    if (backbuf) {
        kfree(backbuf);
        backbuf = NULL;
    }
    kprintf(ANSI_CYAN "[GUI]" ANSI_RESET " Exited FulmenOS Desktop.\n");
}
