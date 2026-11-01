#include "pugfs.h"
#include "path.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#define PUGFS_MAGIC_NUMBER  0xBEEFFEED
#define PUGFS_VERSION       1

#define PUGFS_FILENAME_BYTES         32     // must be padded with '\0'
#define PUGFS_FTABLE_ENTRY_BYTES     4

#define PUGFS_FTABLE_ENTRY_FREE             0x00000000
#define PUGFS_FTABLE_ENTRY_EOC              0xFFFFFFFC  // end of chain
#define PUGFS_FTABLE_ENTRY_RESERVED         0xFFFFFFFD
#define PUGFS_FTABLE_ENTRY_BAD              0xFFFFFFFE
#define PUGFS_FTABLE_ENTRY_INVALID          0xFFFFFFFF
#define PUGFS_FTABLE_ENTRY_CLUSTER_MIN      0x00000001
#define PUGFS_FTABLE_ENTRY_CLUSTER_MAX      0xFFFFFFFB

#define PUGFS_DIR_ENTRY_TYPE_EOD        0xFF    // marks the end of directory, not a real file
#define PUGFS_DIR_ENTRY_TYPE_BAD        0xFE    // bad or corrupted entry
#define PUGFS_DIR_ENTRY_TYPE_DIR        0xFD    // directory
#define PUGFS_DIR_ENTRY_TYPE_FILE       0xFC    // regular file
#define PUGFS_DIR_ENTRY_TYPE_FREE       0xFB    // unused entry, can be reused

struct pugfs_header
{
    uint8_t padding[3];                 // short jump + nop (on bootable disks)
    uint32_t magic;                     // 0xBEEFFEED
    uint16_t version;
    uint16_t sector_size;
    uint16_t sectors_per_cluster;       // for data region
    uint16_t header_size;
    uint16_t flags;
    uint32_t total_sectors;             // total sectors in the image
    uint32_t reserved_sectors;          // number of reserved sectors including the header
    uint32_t ftable_start_sector;       // start of the File Table (LBA)
    uint32_t ftable_sectors;            // size of the FTable
    uint32_t data_start_sector;         // start of the data region (LBA)
    uint32_t data_sectors;              // size of the data region
    uint32_t root_dir_starting_index;   // index into the FTable
    uint32_t checksum;
} __attribute__((packed));

struct pugfs_dir_entry
{
    uint8_t name[PUGFS_FILENAME_BYTES];  // 32 bytes including null terminator
    uint16_t attributes;                 // 2 bytes
    uint8_t type;                        // 1 byte
    uint32_t size;                       // 4 bytes
    uint32_t creation_ts;                // 4 bytes
    uint32_t last_access_ts;             // 4 bytes
    uint32_t starting_cluster_index;     // 4 bytes
    uint8_t reserved[13];                // 13 bytes
} __attribute__((packed));               // total 64 bytes

struct pugfs
{
    struct pugfs_header header;
    struct pugfs_dir_entry root;
    DISK_IMG_STREAM *stream;
    char *buf;
};

// return 0 on equal, <0 if e1 < e2, >0 if e1 > e2
typedef int (*dir_entry_compare_func_t)(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);

// fs validation
static uint32_t _pugfs_header_checksum(struct pugfs_header *header);
static int _pugfs_check_header(struct pugfs_header* header, DISK_IMG *img);

// fs creation
static int _pugfs_calculate_ftable_and_data_sizes(
    uint32_t total_sectors,
    uint32_t reserved_sectors,
    uint16_t sectors_per_cluster,
    uint32_t entries_per_sector,
    uint32_t *ftable_sectors_out,
    uint32_t *data_sectors_out
);
static PUGFS* _pugfs_create(DISK_IMG *img);

// ftable access
static uint32_t _pugfs_read_ftable_entry(PUGFS *fs, uint32_t index);
static int _pugfs_write_ftable_entry(PUGFS *fs, uint32_t index, uint32_t value);

// cluster chain management
static int _pugfs_allocate_cluster(PUGFS *fs, uint32_t *cluster_out);
static int _pugfs_allocate_cluster_chain(PUGFS *fs, uint32_t count, uint32_t *starting_cluster_out);
static int _pugfs_free_cluster_chain(PUGFS *fs, uint32_t starting_cluster_index);

// functions for comparing directory entries
static int _pugfs_compare_dir_entries_by_name(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);
static int _pugfs_compare_dir_entries_by_type(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);

// query functions for directory entries
static int _pugfs_find_dir_entry_in_data_cluster(
    PUGFS *fs,
    uint32_t cluster_index, 
    const struct pugfs_dir_entry *entry_to_find, 
    dir_entry_compare_func_t cmp,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster
);
static int _pugfs_find_dir_entry_in_dir(
    PUGFS *fs,
    const struct pugfs_dir_entry *entry_to_find, 
    dir_entry_compare_func_t cmp,
    const struct pugfs_dir_entry *dir,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
);
static int _pugfs_find_dir_entry_absolute(PUGFS *fs, const char *path, struct pugfs_dir_entry *out_entry);

// write functions for directory entries
static int _pugfs_write_dir_entry_to_data_cluster(
    PUGFS *fs,
    uint32_t cluster_index,
    uint32_t index_in_cluster,
    const struct pugfs_dir_entry *entry
);
static int _pugfs_write_dir_entry_absolute(PUGFS *fs, const char *path, struct pugfs_dir_entry *entry);

// wrapper functions for pugfs operations
static int _pugfs_write_new_dir_entry(PUGFS *fs, const char *path, uint8_t type);

PUGFS* pugfs_format(DISK_IMG *img, uint32_t reserved_sectors, uint16_t sectors_per_cluster) {
    if (!img || reserved_sectors < 1 || sectors_per_cluster < 1) return NULL;

    int disk_sector_size = disk_img_get_sector_size(img);
    if (disk_sector_size <= 0) return NULL;

    if ((uint32_t) disk_sector_size < sizeof(struct pugfs_dir_entry))
        return NULL;

    uint32_t total_sectors = disk_img_get_total_sectors(img);
    // disk is way too small
    if (total_sectors < 100) return NULL;

    if (sectors_per_cluster >= total_sectors) return NULL;

    uint32_t entries_per_sector = (uint32_t)disk_sector_size / PUGFS_FTABLE_ENTRY_BYTES;
    if (entries_per_sector == 0) return NULL;

    uint32_t ftable_sectors = 0, data_sectors = 0;
    int res = _pugfs_calculate_ftable_and_data_sizes(
        total_sectors,
        reserved_sectors,
        sectors_per_cluster,
        entries_per_sector,
        &ftable_sectors,
        &data_sectors
    );
    if (res < 0) return NULL;

    PUGFS *fs = _pugfs_create(img);
    if (!fs) return NULL;

    // create and write the header
    fs->header.magic = PUGFS_MAGIC_NUMBER;
    fs->header.version = PUGFS_VERSION;
    fs->header.sector_size = disk_sector_size;
    fs->header.sectors_per_cluster = sectors_per_cluster;
    fs->header.header_size = sizeof(struct pugfs_header);
    fs->header.flags = 0x0; // unused for now
    fs->header.total_sectors = total_sectors;
    fs->header.reserved_sectors = reserved_sectors;
    fs->header.ftable_start_sector = reserved_sectors;
    fs->header.ftable_sectors = ftable_sectors;
    fs->header.data_start_sector = reserved_sectors + ftable_sectors;
    fs->header.data_sectors = data_sectors;
    fs->header.root_dir_starting_index = 1; // entry 1 in FTable
    fs->header.checksum = _pugfs_header_checksum(&fs->header);
    disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_header), &fs->header);

    // write the FTable
    memset(fs->buf, 0, disk_sector_size);
    *(uint32_t*)(fs->buf) = PUGFS_FTABLE_ENTRY_RESERVED; // entry 0, not used
    *((uint32_t*)(fs->buf) + 1) = PUGFS_FTABLE_ENTRY_EOC; // entry 1, root dir

    disk_img_stream_seek(fs->stream, reserved_sectors, 0);
    disk_img_stream_write_sectors(fs->stream, 1, fs->buf);

    memset(fs->buf, 0, disk_sector_size);
    for (int i = 1; i < ftable_sectors; ++i) {
        disk_img_stream_write_sectors(fs->stream, 1, fs->buf);
    }
    
    // write EOD to root dir (at cluster 1 from the data area)
    struct pugfs_dir_entry eod;
    memset(&eod, 0, sizeof(struct pugfs_dir_entry));
    eod.type = PUGFS_DIR_ENTRY_TYPE_EOD;
    disk_img_stream_seek(fs->stream, reserved_sectors + ftable_sectors, 0);
    disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_dir_entry), &eod);

    return fs;
}

PUGFS* pugfs_load(DISK_IMG *img) {
    if (!img) return NULL;

    PUGFS *fs = _pugfs_create(img);
    if (!fs) return NULL;

    // read boot sector
    int sector_size = disk_img_get_sector_size(img);
    disk_img_stream_read_sectors(fs->stream, 1, fs->buf);

    // copy header
    memcpy(&fs->header, fs->buf, sizeof(struct pugfs_header));

    // check header fields
    if (_pugfs_check_header(&fs->header, img) < 0) {
        pugfs_destroy(fs);
        return NULL;
    }
    
    return fs;
}

void pugfs_destroy(PUGFS *fs) {
    if (fs) {
        if (fs->stream) {
            disk_img_stream_destroy(fs->stream);
        }
        if (fs->buf) {
            free(fs->buf);
        }
        free(fs);
    }
}

int pugfs_touch(PUGFS *fs, const char *path) {
    if (!fs || !path) return -1;

    struct pugfs_dir_entry entry;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry);
    if (res == 0) {
        // file already exists

        // TODO: update its timestamp and rewrite
        return -2;
    }

    return _pugfs_write_new_dir_entry(fs, path, PUGFS_DIR_ENTRY_TYPE_FILE);
}

int pugfs_delete(PUGFS *fs, const char *path) {
    return -1;
}

int pugfs_trunc(PUGFS *fs, const char *path, uint32_t new_size) {
    return -1;
}

int pugfs_mkdir(PUGFS *fs, const char *path) {
    if (!fs || !path) return -1;

    struct pugfs_dir_entry entry;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry);
    if (res == 0) {
        // dir already exists
        return -2;
    }

    return _pugfs_write_new_dir_entry(fs, path, PUGFS_DIR_ENTRY_TYPE_DIR);
}

int pugfs_rmdir(PUGFS *fs, const char *path) {
    return -1;
}

int pugfs_lsdir(PUGFS *fs, const char *path, char **list_out, size_t start_index, size_t max_entries) {
    return -1;
}

int pugfs_copy(PUGFS *fs, const char *src, const char *dst) {
    return -1;
}

int pugfs_move(PUGFS *fs, const char *src, const char *dst) {
    return -1;
}

pugfs_fd pugfs_fopen(PUGFS *fs, const char *path, pugfs_file_mode_t mode) {
    return -1;
}

int pugfs_fclose(PUGFS *fs, pugfs_fd fd) {
    return -1;
}

int pugfs_fstat(PUGFS *fs, pugfs_fd fd, struct pugfs_filestat *stat) {
    return -1;
}

int pugfs_fseek(PUGFS *fs, pugfs_fd fd, int offset, pugfs_seek_mode_t whence) {
    return -1;
}

size_t pugfs_fread(PUGFS *fs, void *dst, size_t size, size_t nmemb, pugfs_fd fd) {
    return 0;
}

size_t pugfs_fwrite(PUGFS *fs, const void *src, size_t size, size_t nmemb, pugfs_fd fd) {
    return 0;
}

static uint32_t _pugfs_header_checksum(struct pugfs_header *header) {
    if (!header) return 0;
    return header->magic ^ header->version ^ header->sector_size ^
        header->header_size ^ header->flags ^ header->total_sectors  ^
        header->total_sectors ^ header->reserved_sectors ^ header->ftable_start_sector ^
        header->ftable_sectors ^ header->data_start_sector ^ header->data_sectors ^
        header->root_dir_starting_index;
}

static int _pugfs_calculate_ftable_and_data_sizes(
    uint32_t total_sectors,
    uint32_t reserved_sectors,
    uint16_t sectors_per_cluster,
    uint32_t entries_per_sector,
    uint32_t *ftable_sectors_out,
    uint32_t *data_sectors_out
) {
    if (sectors_per_cluster == 0 ||
        entries_per_sector == 0 ||
        reserved_sectors >= total_sectors ||
        !ftable_sectors_out ||
        !data_sectors_out) {
        return -1;
    }

    uint32_t available_sectors = total_sectors - reserved_sectors;
    uint32_t data_clusters = available_sectors / sectors_per_cluster;

    for (int i = INT32_MIN; i < INT32_MAX; ++i) {
        uint32_t ftable_entries = data_clusters + 1;

        uint32_t ftable_sectors = ftable_entries / entries_per_sector;
        if (ftable_entries % entries_per_sector != 0) {
            ++ftable_sectors;
        }

        if (ftable_sectors >= available_sectors)
            return -1;

        uint32_t data_sectors = available_sectors - ftable_sectors;
        uint32_t new_data_clusters = data_sectors / sectors_per_cluster;

        if (new_data_clusters >= data_clusters) {
            *ftable_sectors_out = ftable_sectors;
            *data_sectors_out = data_sectors;
            return 0;
        }

        data_clusters = new_data_clusters;
    }
}

static PUGFS* _pugfs_create(DISK_IMG *img) {
    if (!img) return NULL;

    int disk_sector_size = disk_img_get_sector_size(img);
    if (disk_sector_size <= 0) return NULL;

    uint32_t total_sectors = disk_img_get_total_sectors(img);
    // disk is way too small
    if (total_sectors < 100) return NULL;

    PUGFS *fs = (PUGFS*)malloc(sizeof(struct pugfs));
    if (!fs) return NULL;
    memset(fs, 0, sizeof(struct pugfs));

    fs->root.type = PUGFS_DIR_ENTRY_TYPE_DIR;
    fs->root.starting_cluster_index = 1;

    fs->buf = (char*)malloc(disk_sector_size);
    if (!fs->buf) {
        free(fs);
        return NULL;
    }

    fs->stream = disk_img_stream_create(img);
    if (!fs->stream) {
        free(fs->buf);
        free(fs);
        return NULL;
    }
    disk_img_stream_seek(fs->stream, 0, 0);

    return fs;
}

static int _pugfs_check_header(struct pugfs_header* header, DISK_IMG *img) {
    if (!header || !img) return -1;

    // check magic number
    if (header->magic != PUGFS_MAGIC_NUMBER) return -2;

    // verify header size
    if (header->header_size != sizeof(struct pugfs_header)) return -2;

    // verify disk parameters
    int sector_size = disk_img_get_sector_size(img);
    int total_sectors = disk_img_get_total_sectors(img);
    if ((uint32_t)sector_size != header->sector_size || 
        (uint32_t)total_sectors != header->total_sectors)  
        return -2;

    // verify checksum
    int checksum = _pugfs_header_checksum(header);
    if (checksum != header->checksum) return -2;

    // TODO more checks

    return 0; // header ok
}

static uint32_t _pugfs_read_ftable_entry(PUGFS *fs, uint32_t index) {
    if (!fs || index > PUGFS_FTABLE_ENTRY_CLUSTER_MAX) return PUGFS_FTABLE_ENTRY_INVALID;
    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    if (index > data_clusters) return PUGFS_FTABLE_ENTRY_INVALID;

    uint32_t entries_per_sector = fs->header.sector_size / PUGFS_FTABLE_ENTRY_BYTES;
    
    uint32_t sector = fs->header.ftable_start_sector + index / entries_per_sector;
    uint32_t offset = (index % entries_per_sector) * PUGFS_FTABLE_ENTRY_BYTES;
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return PUGFS_FTABLE_ENTRY_INVALID;

    uint32_t entry = PUGFS_FTABLE_ENTRY_INVALID;
    res = disk_img_stream_read_bytes(fs->stream, PUGFS_FTABLE_ENTRY_BYTES, &entry);
    if (res != PUGFS_FTABLE_ENTRY_BYTES) return PUGFS_FTABLE_ENTRY_INVALID;
    
    return entry; 
}

static int _pugfs_write_ftable_entry(PUGFS *fs, uint32_t index, uint32_t value) {
    if (!fs || index > PUGFS_FTABLE_ENTRY_CLUSTER_MAX) return -1;
    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    if (index > data_clusters) return -1;

    uint32_t entries_per_sector = fs->header.sector_size / PUGFS_FTABLE_ENTRY_BYTES;
    
    uint32_t sector = fs->header.ftable_start_sector + index / entries_per_sector;
    uint32_t offset = (index % entries_per_sector) * PUGFS_FTABLE_ENTRY_BYTES;
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return -1;

    res = disk_img_stream_write_bytes(fs->stream, PUGFS_FTABLE_ENTRY_BYTES, &value);
    if (res != PUGFS_FTABLE_ENTRY_BYTES) return -1;
    
    return 0;
}

static int _pugfs_allocate_cluster(PUGFS *fs, uint32_t *cluster_out) {
    if (!fs || !cluster_out) return -1;
    
    uint32_t entries_per_sector = fs->header.sector_size / PUGFS_FTABLE_ENTRY_BYTES;
    uint32_t total_entries = entries_per_sector * fs->header.ftable_sectors;
    
    uint32_t sector = fs->header.ftable_start_sector;
    uint32_t offset = 0;
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return -1;

    for (uint32_t i = 0; i < total_entries; ++i) {
        uint32_t entry = PUGFS_FTABLE_ENTRY_INVALID;
        res = disk_img_stream_read_bytes(fs->stream, PUGFS_FTABLE_ENTRY_BYTES, &entry);
        if (res != PUGFS_FTABLE_ENTRY_BYTES) return -1;

        if (i == 0) continue; // skip entry 0, reserved

        if (entry == PUGFS_FTABLE_ENTRY_FREE) {
            // found a free cluster, allocate it
            if (_pugfs_write_ftable_entry(fs, i, PUGFS_FTABLE_ENTRY_EOC) != 0)
                return -1;

            *cluster_out = i;
            return 0;
        }
    }

    return -1; // no free clusters found
}

static int _pugfs_allocate_cluster_chain(PUGFS *fs, uint32_t count, uint32_t *starting_cluster_out) {

}

static int _pugfs_free_cluster_chain(PUGFS *fs, uint32_t starting_cluster_index) {

}

static int _pugfs_compare_dir_entries_by_name(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2) {
    if (!e1 || !e2) return -1;
    return strncmp((const char*)e1->name, (const char*)e2->name, PUGFS_FILENAME_BYTES);
}

static int _pugfs_compare_dir_entries_by_type(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2) {
    if (!e1 || !e2) return -1;
    return e1->type - e2->type;
}

static int _pugfs_find_dir_entry_in_data_cluster(
    PUGFS *fs,
    uint32_t cluster_index, 
    const struct pugfs_dir_entry *entry_to_find, 
    dir_entry_compare_func_t cmp,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster
) {
    if (!fs || !entry_to_find || !cmp || !out_entry || !out_index_in_cluster) return -1;

    // check if cluster is in data area bounds
    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    if (cluster_index < 1 || cluster_index > data_clusters) return -1;

    // set stream to the starting sector
    uint32_t start_sector = fs->header.data_start_sector + (cluster_index - 1) * fs->header.sectors_per_cluster;
    int res = disk_img_stream_seek(fs->stream, start_sector, 0);
    if (res < 0) return -1;

    uint64_t bytes_per_cluster = (uint64_t)fs->header.sectors_per_cluster * fs->header.sector_size;
    uint32_t entries_per_cluster = bytes_per_cluster / sizeof(struct pugfs_dir_entry);

    // read at most entries_per_cluster or until EOD
    for (uint32_t i = 0; i < entries_per_cluster; ++i) {
        memset(out_entry, 0, sizeof(struct pugfs_dir_entry));

        int res = disk_img_stream_read_bytes(fs->stream, sizeof(struct pugfs_dir_entry), out_entry);
        if (res != sizeof(struct pugfs_dir_entry)) {
            break;
        }

        if (cmp(out_entry, entry_to_find) == 0) {
            *out_index_in_cluster = i;
            return 0;
        }

        // check EOD
        if (out_entry->type == PUGFS_DIR_ENTRY_TYPE_EOD) {
            break;
        }
    }

    return -1;
}

static int _pugfs_find_dir_entry_in_dir(
    PUGFS *fs,
    const struct pugfs_dir_entry *entry_to_find, 
    dir_entry_compare_func_t cmp,
    const struct pugfs_dir_entry *dir,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
) {
    if (!fs || !entry_to_find || !dir || !cmp || !out_entry || !out_index_in_cluster || !out_cluster_index) return -1;
    if (dir->type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    uint32_t cluster_index = dir->starting_cluster_index;

    for (uint32_t i = 0; i < data_clusters; ++i) {  // limit to 'data_clusters' iterations
        if (cluster_index < 1 || cluster_index > data_clusters)
            return -1;

        uint32_t ftable_entry = _pugfs_read_ftable_entry(fs, cluster_index); 

        // invalid entries
        if (ftable_entry == PUGFS_FTABLE_ENTRY_RESERVED ||
            ftable_entry == PUGFS_FTABLE_ENTRY_INVALID ||
            ftable_entry == PUGFS_FTABLE_ENTRY_BAD ||
            ftable_entry == PUGFS_FTABLE_ENTRY_FREE) {
            return -1;
        }

        int res = _pugfs_find_dir_entry_in_data_cluster(
            fs,
            cluster_index,
            entry_to_find,
            cmp,
            out_entry,
            out_index_in_cluster
        );
        if (res == 0) { // found it
            *out_cluster_index = cluster_index;
            return 0;
        }

        // end of cluster chain
        if (ftable_entry == PUGFS_FTABLE_ENTRY_EOC) {
            break;
        }

        // check if next index is valid
        if (ftable_entry < 1 || ftable_entry > data_clusters)
            return -1;

        cluster_index = ftable_entry;
    }

    return -1;
}

static int _pugfs_find_dir_entry_absolute(PUGFS *fs, const char *path, struct pugfs_dir_entry *out_entry) {
    if (!fs || !path || !out_entry) return -1;

    PATH *p = path_parse(path, NULL);
    if (!p) return -1;
    
    // TODO: validate drive number matches between fs and path

    const struct pugfs_dir_entry *parent = &fs->root;
    PATH_TOKEN *tok = path_get_first_token(p);
    if (!tok) { // path is empty, return root dir
        memcpy(out_entry, &fs->root, sizeof(struct pugfs_dir_entry));
        path_destroy(p);
        return 0;
    }

    struct pugfs_dir_entry search_entry;
    memset(&search_entry, 0, sizeof(struct pugfs_dir_entry));

    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = 0;
    while (tok) {
        const char *name = path_get_token_text(tok);
        size_t name_len = strlen(name);

        if (name_len == 0 || name_len >= PUGFS_FILENAME_BYTES) {
            res = -1;
            break;
        }

        memset(&search_entry, 0, sizeof(search_entry));
        memcpy(search_entry.name, name, name_len);

        res = _pugfs_find_dir_entry_in_dir(
            fs, 
            &search_entry, 
            _pugfs_compare_dir_entries_by_name,
            parent,
            out_entry,
            &index_in_cluster,
            &cluster_index
        );
        if (res != 0) break;

        parent = out_entry;
        tok = path_get_next_token(tok);
    }
    
    path_destroy(p);
    return res;
}

static int _pugfs_write_dir_entry_to_data_cluster(
    PUGFS *fs,
    uint32_t cluster_index,
    uint32_t index_in_cluster,
    const struct pugfs_dir_entry *entry
) {
    if (!fs || !entry) return -1;

    // check if cluster is in data area bounds
    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    if (cluster_index < 1 || cluster_index > data_clusters) return -1;

    uint64_t bytes_per_cluster = (uint64_t)fs->header.sectors_per_cluster * fs->header.sector_size;
    uint32_t entries_per_cluster = bytes_per_cluster / sizeof(struct pugfs_dir_entry);

    if (index_in_cluster >= entries_per_cluster) return -1;

    // set stream to the starting sector
    uint32_t start_sector = fs->header.data_start_sector + (cluster_index - 1) * fs->header.sectors_per_cluster;
    int res = disk_img_stream_seek(fs->stream, start_sector, 0);
    if (res < 0) return -1;

    // advance to the index_in_cluster
    disk_img_stream_advance(fs->stream, index_in_cluster * sizeof(struct pugfs_dir_entry));

    // write the entry
    res = disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_dir_entry), entry);
    if (res != sizeof(struct pugfs_dir_entry)) return -1;

    return 0;
}

static int _pugfs_write_dir_entry_absolute(PUGFS *fs, const char *parent_dir_path, struct pugfs_dir_entry *entry) {
    if (!fs || !parent_dir_path || !entry) return -1;
    
    // TODO: validate drive number matches between fs and path

    // find the parent dir
    struct pugfs_dir_entry parent_dir_entry;
    memset(&parent_dir_entry, 0, sizeof(struct pugfs_dir_entry));
    int res = _pugfs_find_dir_entry_absolute(fs, parent_dir_path, &parent_dir_entry);
    if (res != 0 || parent_dir_entry.type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    // find the the first free and the EOD entry in the parent 
    struct pugfs_dir_entry free_entry, eod_entry, search_entry_free, search_entry_eod;
    uint32_t free_index_in_cluster = 0, eod_index_in_cluster = 0, free_cluster_index = 0, eod_cluster_index = 0;
    memset(&free_entry, 0, sizeof(struct pugfs_dir_entry));
    memset(&eod_entry, 0, sizeof(struct pugfs_dir_entry));
    memset(&search_entry_free, 0, sizeof(struct pugfs_dir_entry));
    memset(&search_entry_eod, 0, sizeof(struct pugfs_dir_entry));
    search_entry_free.type = PUGFS_DIR_ENTRY_TYPE_FREE;
    search_entry_eod.type = PUGFS_DIR_ENTRY_TYPE_EOD;

    res = _pugfs_find_dir_entry_in_dir(
        fs, 
        &search_entry_eod, 
        _pugfs_compare_dir_entries_by_type,
        &parent_dir_entry,
        &eod_entry,
        &eod_index_in_cluster,
        &eod_cluster_index
    );
    if (res != 0) return res;   // no EOD entry found, invalid dir

    res = _pugfs_find_dir_entry_in_dir(
        fs, 
        &search_entry_free, 
        _pugfs_compare_dir_entries_by_type,
        &parent_dir_entry,
        &free_entry,
        &free_index_in_cluster,
        &free_cluster_index
    );
    if (res != 0) { // no free entry, will replace the EOD entry
        free_index_in_cluster = eod_index_in_cluster;
        free_cluster_index = eod_cluster_index;
    
        // find space for a new EOD entry
        uint64_t bytes_per_cluster = (uint64_t)fs->header.sectors_per_cluster * fs->header.sector_size;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(struct pugfs_dir_entry);

        if (eod_index_in_cluster + 1 < entries_per_cluster) {
            // there is space in the current cluster for a new EOD entry
            eod_index_in_cluster++;
        } else {
            // need to allocate a new cluster for the new EOD entry
            uint32_t new_cluster_index = 0;
            res = _pugfs_allocate_cluster(fs, &new_cluster_index);
            if (res != 0) return res;

            // update the FTable entry for the current cluster to point to the new cluster
            res = _pugfs_write_ftable_entry(fs, eod_cluster_index, new_cluster_index);
            if (res != 0) return res;

            eod_cluster_index = new_cluster_index;
            eod_index_in_cluster = 0;
        }
    }
    // by this point, the free and eod indices should be set correctly

    // allocate a cluster for the new dir entry
    uint32_t new_entry_cluster_index = 0;
    res = _pugfs_allocate_cluster(fs, &new_entry_cluster_index);
    if (res != 0) return res;
    entry->starting_cluster_index = new_entry_cluster_index;

    // write the new entry to the free entry location
    res = _pugfs_write_dir_entry_to_data_cluster(fs, free_cluster_index, free_index_in_cluster, entry);
    if (res != 0) return res;

    // write a new EOD entry to the new eod location
    struct pugfs_dir_entry new_eod_entry;
    memset(&new_eod_entry, 0, sizeof(struct pugfs_dir_entry));
    new_eod_entry.type = PUGFS_DIR_ENTRY_TYPE_EOD;
    res = _pugfs_write_dir_entry_to_data_cluster(fs, eod_cluster_index, eod_index_in_cluster, &new_eod_entry);
    if (res != 0) return res;    

    // add an eod if the new entry is a directory
    if (entry->type == PUGFS_DIR_ENTRY_TYPE_DIR) {
        res = _pugfs_write_dir_entry_to_data_cluster(fs, new_entry_cluster_index, 0, &new_eod_entry);
        if (res != 0) return res;
    }

    return 0;
}

static int _pugfs_write_new_dir_entry(PUGFS *fs, const char *path, uint8_t type) {
    PATH *p = path_parse(path, NULL);
    if (!p) return -1;

    char new_entry_name[PATH_MAX_TOKEN_LEN + 1] = {0};
    path_pop_last_token(p, new_entry_name); // remove the last token to get the parent directory path

    char parent_dir_path[PATH_MAX_LEN + 1] = {0};
    path_to_string(p, parent_dir_path);
    path_destroy(p);

    struct pugfs_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(struct pugfs_dir_entry));
    new_entry.type = type;
    new_entry.size = fs->header.sectors_per_cluster * fs->header.sector_size; // initial size is one cluster
    new_entry.creation_ts = 0; // TODO: set to current timestamp
    new_entry.last_access_ts = 0; // TODO: set to current timestamp
    new_entry.starting_cluster_index = 0; // will be allocated later
    new_entry.attributes = 0; // no special attributes
    strncpy((char*)new_entry.name, new_entry_name, PUGFS_FILENAME_BYTES - 1);

    return _pugfs_write_dir_entry_absolute(fs, parent_dir_path, &new_entry);
}