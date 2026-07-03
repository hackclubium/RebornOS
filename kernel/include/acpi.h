#ifndef REBORNOS_ACPI_H
#define REBORNOS_ACPI_H

#include <stdint.h>

/* Just enough ACPI to discover how many CPU cores exist and their
 * Local APIC IDs (via the MADT, the "Multiple APIC Description Table")
 * -- no AML interpreter, no other ACPI tables. rsdp_addr comes from
 * boot_info_t (the bootloader found it in UEFI's own Configuration
 * Table -- see boot/src/main.c's find_acpi_rsdp()); the whole low
 * 4 GiB is already identity-mapped (see vmm.c), so that physical
 * address is directly dereferenceable here. */

/* Fills out_ids (up to max_ids entries) with the APIC ID of every
 * enabled Local APIC entry the MADT lists (the BSP we're already
 * running on is one of them) and sets *out_count. Returns 1 on
 * success, 0 if rsdp_addr is 0 or no MADT could be found -- SMP is
 * simply unavailable then, not a fatal error. */
int acpi_find_local_apic_ids(uint64_t rsdp_addr, uint8_t out_ids[], uint32_t max_ids, uint32_t *out_count);

#endif /* REBORNOS_ACPI_H */
