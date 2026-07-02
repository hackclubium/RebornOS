#include <stdint.h>
#include "fat16.h"
#include "blockdev.h"
#include "panic.h"
#include "minilib.h"
#include "heap.h"

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
    uint64_t partition_lba;    /* absolute LBA the FAT16 volume starts at -- see fat16_init() */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size_bytes;
    uint32_t fat_start_sector;
    uint32_t root_dir_start_sector;
    uint32_t root_dir_sectors;
    uint32_t root_dir_entry_count;
    uint32_t first_data_sector;
    uint16_t *fat_cache;       /* the whole first FAT, read once at init */
    uint32_t fat_cache_entries;
} fs;

/* Every sector number computed from the BPB (fat_start_sector,
 * root_dir_start_sector, data clusters, ...) is relative to the start
 * of the FAT16 volume itself -- but the block device reads in
 * absolute disk LBAs, so every read needs partition_lba added first. */
static void read_sectors(uint32_t volume_relative_lba, uint32_t count, void *buf) {
    blockdev_read_sectors(fs.partition_lba + volume_relative_lba, count, buf);
}

static uint32_t cluster_to_sector(uint32_t cluster) {
    return fs.first_data_sector + (cluster - 2) * fs.sectors_per_cluster;
}

static uint32_t fat_next_cluster(uint32_t cluster) {
    if (cluster >= fs.fat_cache_entries) {
        panic("fat16: cluster chain walked off the end of the FAT (cluster %lu)", (uint64_t)cluster);
    }
    return fs.fat_cache[cluster];
}

static int looks_like_fat16_bpb(const fat16_bpb_t *bpb) {
    return bpb->bytes_per_sector == BLOCKDEV_SECTOR_SIZE && bpb->sectors_per_cluster != 0 &&
           bpb->num_fats != 0 && bpb->fat_size_16 != 0 && bpb->root_entry_count != 0;
}

void fat16_init(void) {
    uint8_t boot_sector[BLOCKDEV_SECTOR_SIZE];
    blockdev_read_sectors(0, 1, boot_sector);

    if (boot_sector[510] != 0x55 || boot_sector[511] != 0xAA) {
        panic("fat16_init: missing 0x55AA signature on LBA 0");
    }

    const fat16_bpb_t *bpb = (const fat16_bpb_t *)boot_sector;
    if (!looks_like_fat16_bpb(bpb)) {
        /* LBA 0 isn't a FAT16 boot sector directly -- it's an MBR
         * instead (QEMU's vvfat driver in ":rw:" mode partitions the
         * disk rather than presenting a "superfloppy" with the
         * filesystem starting at LBA 0). Follow the first partition
         * table entry (offset 0x1BE, a 16-byte entry whose LBA-start
         * field sits at offset 8 within it) to find where the real
         * volume begins. */
        uint32_t partition_lba = (uint32_t)boot_sector[0x1BE + 8] |
                                  ((uint32_t)boot_sector[0x1BE + 9] << 8) |
                                  ((uint32_t)boot_sector[0x1BE + 10] << 16) |
                                  ((uint32_t)boot_sector[0x1BE + 11] << 24);
        if (partition_lba == 0) {
            panic("fat16_init: LBA 0 is neither a FAT16 boot sector nor an MBR with a usable partition");
        }

        blockdev_read_sectors(partition_lba, 1, boot_sector);
        bpb = (const fat16_bpb_t *)boot_sector;
        if (!looks_like_fat16_bpb(bpb)) {
            panic("fat16_init: MBR partition 0 (LBA %lu) doesn't look like FAT16 either",
                  (uint64_t)partition_lba);
        }
        fs.partition_lba = partition_lba;
    }

    fs.bytes_per_sector = bpb->bytes_per_sector;
    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.cluster_size_bytes = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    fs.fat_start_sector = bpb->reserved_sector_count;
    fs.root_dir_start_sector = bpb->reserved_sector_count + (uint32_t)bpb->num_fats * bpb->fat_size_16;
    fs.root_dir_entry_count = bpb->root_entry_count;

    uint32_t root_dir_bytes = bpb->root_entry_count * (uint32_t)sizeof(fat16_dirent_t);
    fs.root_dir_sectors = (root_dir_bytes + bpb->bytes_per_sector - 1) / bpb->bytes_per_sector;
    fs.first_data_sector = fs.root_dir_start_sector + fs.root_dir_sectors;

    uint32_t fat_bytes = (uint32_t)bpb->fat_size_16 * bpb->bytes_per_sector;
    fs.fat_cache = (uint16_t *)kmalloc(fat_bytes);
    if (fs.fat_cache == NULL) {
        panic("fat16_init: kmalloc failed for the %lu-byte FAT cache", (uint64_t)fat_bytes);
    }
    read_sectors(fs.fat_start_sector, bpb->fat_size_16, fs.fat_cache);
    fs.fat_cache_entries = fat_bytes / 2;
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

/* Reads the whole root directory into a freshly kmalloc'd buffer.
 * Small enough (a few hundred entries -- 16KiB or so for a typical
 * volume) to just read in one shot rather than streaming sector by
 * sector; caller must kfree() the result. */
static uint8_t *read_root_dir(void) {
    uint8_t *buf = (uint8_t *)kmalloc((uint64_t)fs.root_dir_sectors * fs.bytes_per_sector);
    if (buf == NULL) {
        panic("fat16: kmalloc failed for the root directory buffer");
    }
    read_sectors(fs.root_dir_start_sector, fs.root_dir_sectors, buf);
    return buf;
}

int fat16_open(const char *name, fat16_file_t *out) {
    uint8_t want[11];
    format_83(name, want);

    uint8_t *root = read_root_dir();
    int found = 0;

    for (uint32_t e = 0; e < fs.root_dir_entry_count; e++) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(root + e * sizeof(fat16_dirent_t));
        if (de->name[0] == 0x00) {
            break; /* first byte 0x00 marks the end of the directory */
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
            found = 1;
            break;
        }
    }

    kfree(root);
    return found;
}

uint32_t fat16_read(const fat16_file_t *file, void *buf, uint32_t max_len) {
    uint32_t remaining = file->size < max_len ? file->size : max_len;
    uint32_t total = remaining;
    uint32_t cluster = file->first_cluster;
    uint8_t *dst = (uint8_t *)buf;

    uint8_t *cluster_buf = (uint8_t *)kmalloc(fs.cluster_size_bytes);
    if (cluster_buf == NULL) {
        panic("fat16_read: kmalloc failed for a %lu-byte cluster buffer", (uint64_t)fs.cluster_size_bytes);
    }

    while (remaining > 0 && cluster >= 2 && cluster < FAT16_EOC_MIN) {
        read_sectors(cluster_to_sector(cluster), fs.sectors_per_cluster, cluster_buf);
        uint32_t chunk = fs.cluster_size_bytes < remaining ? fs.cluster_size_bytes : remaining;
        memcpy(dst, cluster_buf, chunk);
        dst += chunk;
        remaining -= chunk;
        cluster = fat_next_cluster(cluster);
    }

    kfree(cluster_buf);
    return total - remaining;
}

uint32_t fat16_list_root(char *buf, uint32_t max_len) {
    uint8_t *root = read_root_dir();
    uint32_t written = 0;

    for (uint32_t e = 0; e < fs.root_dir_entry_count; e++) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(root + e * sizeof(fat16_dirent_t));
        if (de->name[0] == 0x00) {
            break;
        }
        if (de->name[0] == 0xE5 || de->attr == ATTR_LONG_NAME || (de->attr & ATTR_VOLUME_ID)) {
            continue;
        }

        char line[13]; /* 8 + '.' + 3 + '\n' */
        uint32_t li = 0;
        for (int i = 0; i < 8 && de->name[i] != ' '; i++) {
            line[li++] = (char)de->name[i];
        }
        if (de->name[8] != ' ') {
            line[li++] = '.';
            for (int i = 8; i < 11 && de->name[i] != ' '; i++) {
                line[li++] = (char)de->name[i];
            }
        }
        line[li++] = '\n';

        if (written + li >= max_len) {
            break; /* leave room for the trailing NUL below */
        }
        memcpy(buf + written, line, li);
        written += li;
    }

    kfree(root);

    if (max_len > 0) {
        buf[written < max_len ? written : max_len - 1] = '\0';
    }
    return written;
}
