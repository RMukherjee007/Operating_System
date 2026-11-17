#ifndef FULMEN_FB_H
#define FULMEN_FB_H

#include "types.h"

#define FB_PHYS_ADDR  0x43000000UL
#define FB_WIDTH      800U
#define FB_HEIGHT     600U
#define FB_BPP        4U
#define FB_STRIDE       (FB_WIDTH * FB_BPP)
#define FB_HUD_HEIGHT   32U
#define FB_CONSOLE_COLS (FB_WIDTH / 8U)
#define FB_CONSOLE_ROWS ((FB_HEIGHT - FB_HUD_HEIGHT) / 16U)

/* QEMU virt fw_cfg MMIO registers */
#define FW_CFG_BASE   0x09020000UL
#define FW_CFG_DATA   (*(volatile uint64_t *)(FW_CFG_BASE + 0x00))
#define FW_CFG_CTL    (*(volatile uint16_t *)(FW_CFG_BASE + 0x08))
#define FW_CFG_DMA    (*(volatile uint64_t *)(FW_CFG_BASE + 0x10))

bool fb_init(void);
bool fb_is_active(void);
void fb_set_gui_mode(bool enabled);
bool fb_is_gui_mode(void);
void fb_clear(void);
void fb_putc(char c);
void fb_puts(const char *s);
void fb_update_hud(void);
void fb_put_cell(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg);
void fb_set_cursor(uint32_t col, uint32_t row);

/* 2D pixel & text primitives for Graphical Desktop Mode */
void fb_set_render_target(uint32_t *buf);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_present_with_cursor(const uint32_t *backbuf, int32_t mx, int32_t my);
void fb_move_cursor_only(const uint32_t *backbuf, int32_t old_mx, int32_t old_my,
                         int32_t new_mx, int32_t new_my);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t thickness, uint32_t color);
void fb_draw_char_px(uint32_t px, uint32_t py, char c, uint32_t fg, uint32_t bg);
void fb_draw_string_px(uint32_t px, uint32_t py, const char *s, uint32_t fg, uint32_t bg);

extern const uint8_t g_font8x16[128][16];

#endif /* FULMEN_FB_H */
