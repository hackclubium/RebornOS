#include <stdint.h>
#include "acpi.h"
#include "panic.h"
#include "minilib.h"

typedef struct __attribute__((packed)) {
    char     signature[8]; /* "RSD PTR " (note the trailing space) */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision; /* 0 = ACPI 1.0 (20-byte structure), >=2 = ACPI 2.0+ (36 bytes, fields below valid) */
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_entry_header_t;

#define MADT_ENTRY_LOCAL_APIC 0u
#define MADT_LOCAL_APIC_ENABLED (1u << 0)

typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} madt_local_apic_t;

static int checksum_ok(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum == 0;
}

int acpi_find_local_apic_ids(uint64_t rsdp_addr, uint8_t out_ids[], uint32_t max_ids, uint32_t *out_count) {
    *out_count = 0;

    if (rsdp_addr == 0) {
        return 0;
    }
    const rsdp_t *rsdp = (const rsdp_t *)(uintptr_t)rsdp_addr;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        return 0;
    }

    const sdt_header_t *root;
    int is_xsdt;
    if (rsdp->revision >= 2 && checksum_ok(rsdp, rsdp->length)) {
        root = (const sdt_header_t *)(uintptr_t)rsdp->xsdt_address;
        is_xsdt = 1;
    } else {
        root = (const sdt_header_t *)(uintptr_t)rsdp->rsdt_address;
        is_xsdt = 0;
    }
    if (!checksum_ok(root, root->length)) {
        panic("acpi: root system description table failed its checksum");
    }

    const uint8_t *entries_start = (const uint8_t *)root + sizeof(sdt_header_t);
    uint32_t entry_size = is_xsdt ? 8u : 4u;
    uint32_t entry_count = (root->length - (uint32_t)sizeof(sdt_header_t)) / entry_size;

    const sdt_header_t *madt = NULL;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t table_addr = is_xsdt
            ? *(const uint64_t *)(entries_start + (uint64_t)i * entry_size)
            : (uint64_t) * (const uint32_t *)(entries_start + (uint64_t)i * entry_size);
        const sdt_header_t *table = (const sdt_header_t *)(uintptr_t)table_addr;
        if (memcmp(table->signature, "APIC", 4) == 0) {
            madt = table;
            break;
        }
    }
    if (madt == NULL) {
        return 0;
    }

    /* Right after the MADT's own SDT header come its fixed
     * local_apic_address + flags fields (4 bytes each), then a stream
     * of variable-length entries out to the table's own length. */
    const uint8_t *cursor = (const uint8_t *)madt + sizeof(sdt_header_t) + 8u;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    while (cursor < end) {
        const madt_entry_header_t *entry = (const madt_entry_header_t *)cursor;
        if (entry->length == 0) {
            break; /* malformed -- avoid spinning forever */
        }
        if (entry->type == MADT_ENTRY_LOCAL_APIC) {
            const madt_local_apic_t *lapic_entry = (const madt_local_apic_t *)cursor;
            if ((lapic_entry->flags & MADT_LOCAL_APIC_ENABLED) && *out_count < max_ids) {
                out_ids[*out_count] = lapic_entry->apic_id;
                (*out_count)++;
            }
        }
        cursor += entry->length;
    }

    return *out_count > 0;
}
