#ifndef REBORNOS_EFI_MEMORY_MAP_H
#define REBORNOS_EFI_MEMORY_MAP_H

#include <stdint.h>

/* Layout-compatible with UEFI's EFI_MEMORY_DESCRIPTOR (see
 * boot/include/efi.h for the full EFI_MEMORY_TYPE enum) -- just the
 * fields the kernel's physical memory manager actually reads.
 *
 * Per the UEFI spec, always stride through the real memory map using
 * boot_info_t::memory_map_descriptor_size, NEVER sizeof(this struct):
 * firmware is allowed to report a larger descriptor size to leave room
 * for future fields, and OVMF actually does (48 bytes observed here vs.
 * the 40 this struct occupies). */
typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} efi_memory_descriptor_t;

/* The only types treated as free RAM after ExitBootServices. Notably
 * excludes EfiLoaderCode/EfiLoaderData -- that's where our own kernel
 * image and the boot_info/memory-map buffers themselves live, so
 * treating those as free would let the allocator hand out memory the
 * kernel is currently running out of. */
#define EFI_MEM_TYPE_BOOT_SERVICES_CODE 3u
#define EFI_MEM_TYPE_BOOT_SERVICES_DATA 4u
#define EFI_MEM_TYPE_CONVENTIONAL       7u

#endif /* REBORNOS_EFI_MEMORY_MAP_H */
