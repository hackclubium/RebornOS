#ifndef REBORNOS_FONT8X8_H
#define REBORNOS_FONT8X8_H

#include <stdint.h>

/* Returns 8 rows of 8 bits each (bit 7 = leftmost pixel) for the given
 * character. Original, hand-authored glyph table -- covers uppercase
 * A-Z, digits, space, and common punctuation; anything else (including
 * lowercase, which is folded to uppercase by the caller) falls back to
 * a hollow-box placeholder glyph. Good enough to prove framebuffer text
 * rendering works; a real font can replace this later. */
const uint8_t *font8x8_get_glyph(char c);

#endif /* REBORNOS_FONT8X8_H */
