#include "framebuffer.h"
#include "font8x8.h"

static boot_framebuffer_t g_fb;

void fb_init(const boot_framebuffer_t *fb) {
    g_fb = *fb;
}

static inline uint32_t pack_color(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return ((uint32_t)r << g_fb.red_shift) | ((uint32_t)g << g_fb.green_shift) | ((uint32_t)b << g_fb.blue_shift);
}

static inline void put_pixel(uint32_t x, uint32_t y, uint32_t native_color) {
    if (x >= g_fb.width || y >= g_fb.height) {
        return;
    }
    volatile uint32_t *row = (volatile uint32_t *)(uintptr_t)(g_fb.base + (uint64_t)y * g_fb.pixels_per_scanline * g_fb.bytes_per_pixel);
    row[x] = native_color;
}

void fb_clear(uint32_t rgb) {
    uint32_t native = pack_color(rgb);
    for (uint32_t y = 0; y < g_fb.height; y++) {
        for (uint32_t x = 0; x < g_fb.width; x++) {
            put_pixel(x, y, native);
        }
    }
}

void fb_putc(uint32_t x, uint32_t y, char c, uint32_t fg_rgb, uint32_t bg_rgb) {
    const uint8_t *glyph = font8x8_get_glyph(c);
    uint32_t fg = pack_color(fg_rgb);
    uint32_t bg = pack_color(bg_rgb);

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int set = bits & (0x80 >> col);
            put_pixel(x + (uint32_t)col, y + (uint32_t)row, set ? fg : bg);
        }
    }
}

void fb_puts(uint32_t x, uint32_t y, const char *s, uint32_t fg_rgb, uint32_t bg_rgb) {
    uint32_t cursor_x = x;
    for (const char *p = s; *p; p++) {
        fb_putc(cursor_x, y, *p, fg_rgb, bg_rgb);
        cursor_x += 8;
    }
}
