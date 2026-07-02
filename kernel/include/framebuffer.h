#ifndef REBORNOS_FRAMEBUFFER_H
#define REBORNOS_FRAMEBUFFER_H

#include "boot_info.h"

void fb_init(const boot_framebuffer_t *fb);
void fb_clear(uint32_t rgb);
void fb_putc(uint32_t x, uint32_t y, char c, uint32_t fg_rgb, uint32_t bg_rgb);
void fb_puts(uint32_t x, uint32_t y, const char *s, uint32_t fg_rgb, uint32_t bg_rgb);

#endif /* REBORNOS_FRAMEBUFFER_H */
