#include <stdint.h>
#include "fat16.h"
#include "panic.h"
#include "minilib.h"

/* BPB fields we actually read, at their real on-disk byte offsets.
 * Packed so the compiler doesn't insert padding that would break the
 * offsets -- this is read directly out of the raw disk image bytes,
 * same technique as the UEFI/GDT/TSS structs elsewhere in this
 * codebase. Bytes 0-35 (up to and including TotalSectors32) are shared
 * across every FAT variant; everything from DriveNumber onward is
 * FAT12/16's "extended BPB" layout specifically (FAT32's differs). */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16; /* 0 if the volume is too big for 16 bits -- see total_sectors_32 */
    uint8_t  media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} fat16_bpb_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[11]; /* 8.3, space-padded, no dot */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi; /* reserved (always 0) on FAT12/16 */
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} fat16_dirent_t;

#define ATTR_LONG_NAME 0x0Fu /* LFN entry -- we only support 8.3 names, skip these */
#define ATTR_VOLUME_ID 0x08u
#define FAT16_EOC_MIN  0xFFF8u /* cluster values >= this mean "end of chain" */

static struct {
    const uint8_t *disk;
    uint64_t disk_size;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size_bytes;
    uint32_t fat_start_sector;
    uint32_t root_dir_start_sector;
    uint32_t root_dir_entry_count;
    uint32_t first_data_sector;
} fs;

static const uint8_t *sector_ptr(uint32_t sector) {
    return fs.disk + (uint64_t)sector * fs.bytes_per_sector;
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return fs.first_data_sector + (cluster - 2) * fs.sectors_per_cluster;
}

static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t fat_byte_offset = cluster * 2;
    uint32_t fat_sector = fs.fat_start_sector + (fat_byte_offset / fs.bytes_per_sector);
    uint32_t offset_in_sector = fat_byte_offset % fs.bytes_per_sector;
    const uint8_t *p = sector_ptr(fat_sector) + offset_in_sector;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

void fat16_init(const uint8_t *disk, uint64_t disk_size) {
    if (disk_size < 512) {
        panic("fat16_init: disk image too small to hold a boot sector");
    }
    if (disk[510] != 0x55 || disk[511] != 0xAA) {
        panic("fat16_init: missing 0x55AA boot sector signature");
    }

    const fat16_bpb_t *bpb = (const fat16_bpb_t *)disk;
    if (bpb->bytes_per_sector == 0 || bpb->sectors_per_cluster == 0 || bpb->num_fats == 0 ||
        bpb->fat_size_16 == 0 || bpb->root_entry_count == 0) {
        panic("fat16_init: BPB doesn't look like FAT16 (check the vvfat drive is forced to fat:16:)");
    }

    fs.disk = disk;
    fs.disk_size = disk_size;
    fs.bytes_per_sector = bpb->bytes_per_sector;
    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.cluster_size_bytes = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    fs.fat_start_sector = bpb->reserved_sector_count;
    fs.root_dir_start_sector = bpb->reserved_sector_count + (uint32_t)bpb->num_fats * bpb->fat_size_16;
    fs.root_dir_entry_count = bpb->root_entry_count;

    uint32_t root_dir_bytes = bpb->root_entry_count * (uint32_t)sizeof(fat16_dirent_t);
    uint32_t root_dir_sectors = (root_dir_bytes + bpb->bytes_per_sector - 1) / bpb->bytes_per_sector;
    fs.first_data_sector = fs.root_dir_start_sector + root_dir_sectors;
}

/* Converts "init.elf" (or "INIT.ELF") into the space-padded, dotless
 * 11-byte short name FAT directory entries actually store on disk. */
static void format_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    int oi = 0;
    const char *p = name;
    while (*p != '\0' && *p != '.' && oi < 8) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[oi++] = (uint8_t)c;
    }
    while (*p != '\0' && *p != '.') {
        p++;
    }
    if (*p == '.') {
        p++;
        int ei = 8;
        while (*p != '\0' && ei < 11) {
            char c = *p++;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            out[ei++] = (uint8_t)c;
        }
    }
}

int fat16_open(const char *name, fat16_file_t *out) {
    uint8_t want[11];
    format_83(name, want);

    /* Unlike FAT32, FAT16's root directory is a fixed-size area right
     * after the FATs -- not addressed by cluster number, so there's no
     * chain to walk here. */
    const uint8_t *root = sector_ptr(fs.root_dir_start_sector);
    for (uint32_t e = 0; e < fs.root_dir_entry_count; e++) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(root + e * sizeof(fat16_dirent_t));
        if (de->name[0] == 0x00) {
            return 0; /* first byte 0x00 marks the end of the directory */
        }
        if (de->name[0] == 0xE5 || de->attr == ATTR_LONG_NAME || (de->attr & ATTR_VOLUME_ID)) {
            continue; /* deleted entry, LFN fragment, or the volume label */
        }

        int match = 1;
        for (int i = 0; i < 11; i++) {
            if (de->name[i] != want[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            out->first_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
            out->size = de->file_size;
            return 1;
        }
    }
    return 0;
}

uint32_t fat16_read(const fat16_file_t *file, void *buf, uint32_t max_len) {
    uint32_t remaining = file->size < max_len ? file->size : max_len;
    uint32_t total = remaining;
    uint32_t cluster = file->first_cluster;
    uint8_t *dst = (uint8_t *)buf;

    while (remaining > 0 && cluster >= 2 && cluster < FAT16_EOC_MIN) {
        uint32_t sector = cluster_to_sector(cluster);
        uint32_t chunk = fs.cluster_size_bytes < remaining ? fs.cluster_size_bytes : remaining;
        memcpy(dst, sector_ptr(sector), chunk);
        dst += chunk;
        remaining -= chunk;
        cluster = fat_next_cluster(cluster);
    }
    return total - remaining;
}
