#ifndef REBORNOS_FRAMEBUFFER_H
#define REBORNOS_FRAMEBUFFER_H

#include "boot_info.h"

void fb_init(const boot_framebuffer_t *fb);
void fb_clear(uint32_t rgb);
void fb_putc(uint32_t x, uint32_t y, char c, uint32_t fg_rgb, uint32_t bg_rgb);
void fb_puts(uint32_t x, uint32_t y, const char *s, uint32_t fg_rgb, uint32_t bg_rgb);

/* The full boot-time framebuffer descriptor (base/size included) --
 * for kernel-internal callers only (see syscall.c's SYS_FB_INFO/
 * SYS_FB_MAP, which copy out only the fields a user process actually
 * needs and never leak the raw physical base/size to userspace). */
const boot_framebuffer_t *fb_get_info(void);

/* Packs a 0xRRGGBB color the same way fb_clear()/fb_putc() do
 * internally -- exposed so a self-test can compute the exact native
 * pixel value a user program should have written and compare it
 * against fb_read_pixel() below. */
uint32_t fb_pack_color(uint32_t rgb);

/* Reads back the raw native pixel value at (x, y) directly from
 * framebuffer memory -- lets a self-test verify a write a *user*
 * process made through its own SYS_FB_MAP mapping actually landed in
 * the same memory these kernel-side drawing functions use, rather
 * than some private copy. */
uint32_t fb_read_pixel(uint32_t x, uint32_t y);

#endif /* REBORNOS_FRAMEBUFFER_H */
