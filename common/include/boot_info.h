#ifndef REBORNOS_BOOT_INFO_H
#define REBORNOS_BOOT_INFO_H

#include <stdint.h>

/* Handoff contract between boot/ and kernel/. Both sides include this
 * header, so a field change here must be made consistent on both sides
 * of the jump in the same commit. */

#define BOOT_INFO_MAGIC 0x5245424F524E4F53ULL /* ASCII-ish "REBORNOS" sentinel */

typedef struct {
    uint64_t base;               /* physical address of the pixel buffer */
    uint64_t size;                /* size of the buffer in bytes */
    uint32_t width;               /* horizontal resolution in pixels */
    uint32_t height;              /* vertical resolution in pixels */
    uint32_t pixels_per_scanline; /* may exceed width (GOP padding) */
    uint32_t bytes_per_pixel;     /* always 4 in Milestone 0 (32bpp) */
    uint8_t  red_shift;           /* bit offset of the red channel within a pixel */
    uint8_t  green_shift;
    uint8_t  blue_shift;
} boot_framebuffer_t;

typedef struct {
    uint64_t magic;
    boot_framebuffer_t framebuffer;

    /* Raw UEFI memory map, copied as-is. The kernel does not parse this
     * in Milestone 0 (no allocator yet) -- it is captured now so the
     * memory-management milestone doesn't need to change the boot
     * handoff contract. */
    uint64_t memory_map_addr;
    uint64_t memory_map_size;
    uint64_t memory_map_descriptor_size;
    uint32_t memory_map_descriptor_version;
} boot_info_t;

#endif /* REBORNOS_BOOT_INFO_H */
