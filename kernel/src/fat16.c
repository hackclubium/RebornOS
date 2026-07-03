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

#define ATTR_LONG_NAME  0x0Fu /* LFN entry -- we only support 8.3 names, skip these */
#define ATTR_VOLUME_ID  0x08u
#define ATTR_DIRECTORY  0x10u
#define ATTR_ARCHIVE    0x20u
#define FAT16_EOC_MIN   0xFFF8u /* cluster values >= this mean "end of chain" when reading */
#define FAT16_EOC_MARK  0xFFFFu /* the value we write to mark a chain's last cluster */
#define FAT16_FREE_MARK 0x0000u

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
    uint16_t *fat_cache;       /* the whole first FAT, read once at init, mutated in place on write */
    uint32_t fat_cache_entries;
} fs;

/* Every sector number computed from the BPB (fat_start_sector,
 * root_dir_start_sector, data clusters, ...) is relative to the start
 * of the FAT16 volume itself -- but the block device reads/writes in
 * absolute disk LBAs, so every access needs partition_lba added first. */
static void read_sectors(uint32_t volume_relative_lba, uint32_t count, void *buf) {
    blockdev_read_sectors(fs.partition_lba + volume_relative_lba, count, buf);
}

static void write_sectors(uint32_t volume_relative_lba, uint32_t count, const void *buf) {
    blockdev_write_sectors(fs.partition_lba + volume_relative_lba, count, buf);
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

/* Reads a subdirectory's entire cluster chain into a freshly kmalloc'd
 * buffer -- unlike the root directory, a subdirectory's size isn't
 * stored anywhere, so this walks the chain once to count clusters,
 * then again to actually read them. Directories are small enough in
 * practice that this two-pass approach is simpler than growing a
 * buffer incrementally. Caller must kfree() the result. */
static uint8_t *read_subdir_clusters(uint32_t first_cluster, uint32_t *out_total_bytes) {
    uint32_t count = 0;
    for (uint32_t c = first_cluster; c >= 2 && c < FAT16_EOC_MIN; c = fat_next_cluster(c)) {
        count++;
    }

    uint32_t total = count * fs.cluster_size_bytes;
    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (buf == NULL) {
        panic("fat16: kmalloc failed for a %lu-byte directory buffer", (uint64_t)total);
    }

    uint8_t *dst = buf;
    for (uint32_t c = first_cluster; c >= 2 && c < FAT16_EOC_MIN; c = fat_next_cluster(c)) {
        read_sectors(cluster_to_sector(c), fs.sectors_per_cluster, dst);
        dst += fs.cluster_size_bytes;
    }

    *out_total_bytes = total;
    return buf;
}

/* dir_cluster == 0 is the sentinel for "the root directory" (real data
 * clusters start at 2, so 0 can never collide with one). Fills
 * *out_entry_count and returns a kmalloc'd buffer the caller must
 * kfree(). */
static uint8_t *read_directory(uint32_t dir_cluster, uint32_t *out_entry_count) {
    if (dir_cluster == 0) {
        *out_entry_count = fs.root_dir_entry_count;
        return read_root_dir();
    }
    uint32_t total_bytes;
    uint8_t *buf = read_subdir_clusters(dir_cluster, &total_bytes);
    *out_entry_count = total_bytes / (uint32_t)sizeof(fat16_dirent_t);
    return buf;
}

/* Scans a directory (see read_directory()) for an 8.3 name, skipping
 * deleted entries, LFN fragments, and the volume label. Returns 1 and
 * copies the matching entry into *out on success. */
static int find_entry_in_dir(uint32_t dir_cluster, const uint8_t want[11], fat16_dirent_t *out) {
    uint32_t entry_count;
    uint8_t *dirbuf = read_directory(dir_cluster, &entry_count);
    int found = 0;

    for (uint32_t e = 0; e < entry_count; e++) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(dirbuf + e * sizeof(fat16_dirent_t));
        if (de->name[0] == 0x00) {
            break; /* first byte 0x00 marks the end of the directory */
        }
        if (de->name[0] == 0xE5 || de->attr == ATTR_LONG_NAME || (de->attr & ATTR_VOLUME_ID)) {
            continue;
        }
        int match = 1;
        for (int i = 0; i < 11; i++) {
            if (de->name[i] != want[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            *out = *de;
            found = 1;
            break;
        }
    }

    kfree(dirbuf);
    return found;
}

/* Splits `path` on '/' and, for every component except the last,
 * resolves it as a subdirectory of the directory found so far
 * (starting at root). Fills *out_dir_cluster with the cluster of the
 * directory that should contain the final component, and leaf_out
 * (>= 13 bytes) with that final component. Returns 0 if any
 * intermediate component doesn't exist or isn't a directory. */
static int resolve_parent_dir(const char *path, uint32_t *out_dir_cluster, char leaf_out[13]) {
    uint32_t current_dir = 0; /* root */
    const char *p = path;

    for (;;) {
        char component[13];
        int ci = 0;
        while (*p != '\0' && *p != '/' && ci < 12) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';
        while (*p == '/') {
            p++;
        }

        if (*p == '\0') {
            *out_dir_cluster = current_dir;
            for (int i = 0; i <= ci; i++) {
                leaf_out[i] = component[i];
            }
            return 1;
        }

        uint8_t want[11];
        format_83(component, want);
        fat16_dirent_t entry;
        if (!find_entry_in_dir(current_dir, want, &entry) || !(entry.attr & ATTR_DIRECTORY)) {
            return 0;
        }
        current_dir = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
    }
}

/* Like resolve_parent_dir(), but resolves every component (including
 * the last) as a directory -- for listing a directory itself rather
 * than finding a file inside one. An empty path means the root. */
static int resolve_directory(const char *path, uint32_t *out_cluster) {
    if (path == NULL || path[0] == '\0') {
        *out_cluster = 0;
        return 1;
    }

    uint32_t current_dir = 0;
    const char *p = path;

    for (;;) {
        char component[13];
        int ci = 0;
        while (*p != '\0' && *p != '/' && ci < 12) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';
        while (*p == '/') {
            p++;
        }

        if (ci == 0) { /* trailing slash */
            *out_cluster = current_dir;
            return 1;
        }

        uint8_t want[11];
        format_83(component, want);
        fat16_dirent_t entry;
        if (!find_entry_in_dir(current_dir, want, &entry) || !(entry.attr & ATTR_DIRECTORY)) {
            return 0;
        }
        current_dir = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;

        if (*p == '\0') {
            *out_cluster = current_dir;
            return 1;
        }
    }
}

int fat16_open(const char *path, fat16_file_t *out) {
    uint32_t dir_cluster;
    char leaf[13];
    if (!resolve_parent_dir(path, &dir_cluster, leaf)) {
        return 0;
    }

    uint8_t want[11];
    format_83(leaf, want);
    fat16_dirent_t entry;
    if (!find_entry_in_dir(dir_cluster, want, &entry)) {
        return 0;
    }

    out->first_cluster = ((uint32_t)entry.first_cluster_hi << 16) | entry.first_cluster_lo;
    out->size = entry.file_size;
    return 1;
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

int32_t fat16_list_dir(const char *path, char *buf, uint32_t max_len) {
    uint32_t dir_cluster;
    if (!resolve_directory(path, &dir_cluster)) {
        return -1;
    }

    uint32_t entry_count;
    uint8_t *dirbuf = read_directory(dir_cluster, &entry_count);
    uint32_t written = 0;

    for (uint32_t e = 0; e < entry_count; e++) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(dirbuf + e * sizeof(fat16_dirent_t));
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

    kfree(dirbuf);

    if (max_len > 0) {
        buf[written < max_len ? written : max_len - 1] = '\0';
    }
    return (int32_t)written;
}

static uint32_t alloc_cluster(void) {
    for (uint32_t c = 2; c < fs.fat_cache_entries; c++) {
        if (fs.fat_cache[c] == FAT16_FREE_MARK) {
            return c;
        }
    }
    panic("fat16: disk full (no free clusters)");
}

static void write_fat_cache_to_disk(void) {
    uint32_t fat_bytes = fs.fat_cache_entries * 2;
    uint32_t fat_sectors = fat_bytes / fs.bytes_per_sector; /* exact -- came from a whole-sector read at init */
    write_sectors(fs.fat_start_sector, fat_sectors, fs.fat_cache);
}

int fat16_write_file(const char *name, const void *data, uint32_t size) {
    uint8_t want[11];
    format_83(name, want);

    uint8_t *root = read_root_dir();

    /* Find an existing entry with this name (to overwrite -- its old
     * clusters get freed below) or, failing that, the first free slot
     * (a deleted entry, or the boundary marking "never used" beyond). */
    int existing_index = -1;
    int free_index = -1;
    for (uint32_t e = 0; e < fs.root_dir_entry_count; e++) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(root + e * sizeof(fat16_dirent_t));
        if (de->name[0] == 0x00) {
            if (free_index < 0) {
                free_index = (int)e;
            }
            break;
        }
        if (de->name[0] == 0xE5) {
            if (free_index < 0) {
                free_index = (int)e;
            }
            continue;
        }
        if (de->attr == ATTR_LONG_NAME || (de->attr & ATTR_VOLUME_ID)) {
            continue;
        }
        int match = 1;
        for (int i = 0; i < 11; i++) {
            if (de->name[i] != want[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            existing_index = (int)e;
            break;
        }
    }

    int slot = existing_index >= 0 ? existing_index : free_index;
    if (slot < 0) {
        kfree(root);
        panic("fat16_write_file: root directory is full");
    }

    if (existing_index >= 0) {
        const fat16_dirent_t *de = (const fat16_dirent_t *)(root + (uint32_t)existing_index * sizeof(fat16_dirent_t));
        uint32_t old_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
        while (old_cluster >= 2 && old_cluster < FAT16_EOC_MIN) {
            uint32_t next = fs.fat_cache[old_cluster];
            fs.fat_cache[old_cluster] = FAT16_FREE_MARK;
            old_cluster = next;
        }
    }

    uint32_t clusters_needed = size == 0 ? 0 : (size + fs.cluster_size_bytes - 1) / fs.cluster_size_bytes;
    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t remaining = size;
    uint8_t *cluster_buf = clusters_needed > 0 ? (uint8_t *)kmalloc(fs.cluster_size_bytes) : NULL;

    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t c = alloc_cluster();
        fs.fat_cache[c] = (uint16_t)FAT16_EOC_MARK; /* provisional -- overwritten below if another cluster follows */
        if (prev_cluster == 0) {
            first_cluster = c;
        } else {
            fs.fat_cache[prev_cluster] = (uint16_t)c;
        }
        prev_cluster = c;

        uint32_t chunk = fs.cluster_size_bytes < remaining ? fs.cluster_size_bytes : remaining;
        memset(cluster_buf, 0, fs.cluster_size_bytes);
        memcpy(cluster_buf, src, chunk);
        write_sectors(cluster_to_sector(c), fs.sectors_per_cluster, cluster_buf);

        src += chunk;
        remaining -= chunk;
    }
    if (cluster_buf != NULL) {
        kfree(cluster_buf);
    }

    fat16_dirent_t *de = (fat16_dirent_t *)(root + (uint32_t)slot * sizeof(fat16_dirent_t));
    memset(de, 0, sizeof(*de));
    memcpy(de->name, want, 11);
    de->attr = ATTR_ARCHIVE;
    de->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de->file_size = size;

    /* Only the sector containing this one directory entry actually
     * changed -- write that back, plus the FAT cache (mutated above by
     * freeing the old chain and/or allocating a new one). */
    uint32_t entries_per_sector = fs.bytes_per_sector / (uint32_t)sizeof(fat16_dirent_t);
    uint32_t sector_index = (uint32_t)slot / entries_per_sector;
    write_sectors(fs.root_dir_start_sector + sector_index, 1, root + sector_index * fs.bytes_per_sector);
    write_fat_cache_to_disk();

    kfree(root);
    return 1;
}
