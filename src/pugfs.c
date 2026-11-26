#include "pugfs.h"
#include "path.h"

#include <stdio.h>
#include <string.h>

#define PUGFS_MAGIC_NUMBER  0xBEEFFEED
#define PUGFS_VERSION       1

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

#define PUGFS_DIR_ITERATOR_FLAG_BEGIN       0b00000001
#define PUGFS_DIR_ITERATOR_FLAG_END         0b00000010

#define PUGFS_MAX_FILE_DESCRIPTORS  32

#define PUGFS_RW_AT_CLUSTER_MODE_READ   0
#define PUGFS_RW_AT_CLUSTER_MODE_WRITE  1

// raw header format
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

// raw dir entry format
struct pugfs_dir_entry
{
    uint8_t name[PUGFS_FILENAME_BYTES];  // 32 bytes including null terminator
    uint16_t attributes;                 // 2 bytes
    uint8_t type;                        // 1 byte
    uint32_t size_lo;                    // 4 bytes
    uint32_t size_hi;                    // 4 bytes
    uint32_t creation_ts;                // 4 bytes
    uint32_t last_access_ts;             // 4 bytes
    uint32_t starting_cluster_index;     // 4 bytes
    uint8_t reserved[9];                 // 9 bytes
} __attribute__((packed));               // total 64 bytes

// commonly used pre-calculated values
struct pugfs_params
{
    uint32_t data_clusters;
    uint32_t entries_per_sector;
    uint64_t bytes_per_cluster;
    uint32_t entries_per_cluster;
};

// information for opened files
struct pugfs_file_descriptor
{
    pugfs_fd fd; // for validation
    pugfs_file_mode_t mode;
    uint64_t byte_pos;
    struct pugfs_dir_entry entry;
    uint32_t cluster_index;
    uint32_t index_in_cluster;
};

/* 
    Dir entry iteratior with cluster chain traversal
    Usage:
        - allocate an iter object
        - call iter_begin once
        - call iter_next repeatedly
*/
struct pugfs_dir_iterator
{
    // public fields - access freely from outside iter_next
    struct pugfs_dir_entry current;         // copy of current entry, overwritten inside iter_next
    uint32_t current_cluster_index;         // cluster index of current entry (for writing back)
    uint32_t current_index_in_cluster;      // index in cluster of current entry (for writing back)

    // private fields - should not be accessed outside of iter_begin/iter_next
    struct pugfs_dir_entry _parent;         // set inside iter_begin
    uint8_t _flags;                         // set inside iter_begin or iter_end
    uint32_t _clusters_visited;             // counts visited clusters to prevent infinite loops in case of corrupted chains
    uint32_t _next_cluster_index;           // cluster index for next iteration
    uint32_t _next_index_in_cluster;        // index in cluster for next iteration
};

// fs instance
struct pugfs
{
    int drive_no;
    struct pugfs_header header;
    struct pugfs_params params;
    struct pugfs_dir_entry root;
    struct pugfs_file_descriptor file_descriptors[PUGFS_MAX_FILE_DESCRIPTORS];
    struct pugfs_dir_iterator lsdir;    // for lsdir function
    DISK_IMG_STREAM *stream;
    char *buf;
};

// used to match entries in query functions
struct pugfs_de_comparator
{
    // return 0 on equal, <0 if e1 < e2, >0 if e1 > e2
    int (*cmp_func)(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);
    struct pugfs_dir_entry cmp_entry; // can be optionally used as e2 param
};

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
static void _pugfs_set_fs_params(PUGFS *fs); // call only after loading the header

// ftable access
static uint32_t _pugfs_read_ftable_entry(PUGFS *fs, uint32_t index);
static int _pugfs_write_ftable_entry(PUGFS *fs, uint32_t index, uint32_t value);

// cluster chain management
static int _pugfs_allocate_cluster(PUGFS *fs, uint32_t *cluster_out);
static int _pugfs_free_cluster_chain(PUGFS *fs, uint32_t starting_cluster);
static int _pugfs_copy_cluster_data(PUGFS *fs, uint32_t src_cluster, uint32_t dst_cluster);
static int _pugfs_duplicate_cluster_chain(PUGFS *fs, uint32_t src_start_cluster, uint32_t *dst_start_cluster_out);
static int _pugfs_get_next_cluster_in_chain(PUGFS *fs, uint32_t cluster, uint32_t *next_cluster_out);
static int _pugfs_get_cluster_in_chain_at_byte_offset(
    PUGFS *fs, 
    uint32_t starting_cluster,
    uint64_t byte_offset, 
    uint32_t *cluster_out,
    uint32_t *prev_cluster_out
);

// functions for comparing directory entries
static int _pugfs_compare_dir_entries_by_name(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);
static int _pugfs_compare_dir_entries_by_type(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);
static int _pugfs_compare_dir_entries_file_or_dir(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry*);

// query functions for directory entries
static int _pugfs_find_dir_entry_in_dir(    // find the first matching entry
    PUGFS *fs,
    const struct pugfs_de_comparator *comparator,
    const struct pugfs_dir_entry *dir,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
);
static int _pugfs_find_dir_entry_absolute(  // find entry by absolute path
    PUGFS *fs, 
    PATH *path, 
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
);

// write functions for directory entries
static int _pugfs_write_dir_entry_to_data_cluster(  // write entry to a specific index inside a data cluster
    PUGFS *fs,
    uint32_t cluster_index,
    uint32_t index_in_cluster,
    const struct pugfs_dir_entry *entry
);
static int _pugfs_write_new_dir_entry_to_dir(   // create a new entry in a directory
    PUGFS *fs, 
    const struct pugfs_dir_entry *dir, 
    struct pugfs_dir_entry *entry,      // 1 cluster will be allocated if entry.type != FREE
    uint32_t *out_index_in_cluster, 
    uint32_t *out_cluster_index
);
static int _pugfs_create_new_dir_entry_absolute(    // create a new entry at an absolute path
    PUGFS *fs, 
    PATH *path,
    uint8_t type,                       // 1 cluster will be allocated if type != FREE
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index
);

// functions for reading/writing raw data clusters
int _pugfs_rw_bytes_at_cluster(
    PUGFS *fs, 
    uint32_t cluster, 
    uint32_t offset, 
    uint32_t count, 
    void *iobuf, 
    uint8_t mode, // mode = 0 => read, mode = 1 => write
    uint32_t *count_out
);
int _pugfs_read_blocks_from_cluster_chain(
    PUGFS *fs, 
    uint32_t starting_cluster,
    uint64_t byte_offset, 
    uint32_t bs, 
    uint32_t count, 
    void *out,
    uint32_t *blocks_read
);
int _pugfs_write_blocks_to_cluster_chain(
    PUGFS *fs, 
    uint32_t starting_cluster, 
    uint64_t byte_offset,
    uint32_t bs, 
    uint32_t count, 
    const void *in,
    uint32_t *blocks_written
);

// utility functions
static int _pugfs_path_is_valid(PUGFS *fs, PATH *path); // 1 on valid, 0 on invalid
static int _pugfs_get_dir_entry_stats(const struct pugfs_dir_entry *entry, struct pugfs_filestat *out);
static int _pugfs_dir_is_empty(PUGFS *fs, const struct pugfs_dir_entry *dir); // 1 if empty, 0 if not, <0 on error
static int _pugfs_copy_dir_contents_rec(PUGFS *fs, const struct pugfs_dir_entry *src, const struct pugfs_dir_entry *dst);
static int _pugfs_iter_begin(struct pugfs_dir_iterator *iter, const struct pugfs_dir_entry *dir);
static int _pugfs_iter_end(struct pugfs_dir_iterator *iter);
// return 1 if has next, 0 if EOD, -1 on error, -2 on EOC (and not EOD)
static int _pugfs_iter_next(PUGFS *fs, struct pugfs_dir_iterator *iter);

// file IO functions
static int _pugfs_alloc_fd(
    PUGFS *fs, 
    const struct pugfs_dir_entry *entry, 
    pugfs_file_mode_t mode, 
    uint32_t cluster_index, 
    uint32_t index_in_cluster
);
static int _pugfs_free_fd(PUGFS *fs, pugfs_fd fd);

static inline uint64_t _pugfs_get_entry_size(const struct pugfs_dir_entry *entry) {
    return ((uint64_t)entry->size_hi << 32) | entry->size_lo;
}

static inline void _pugfs_set_entry_size(struct pugfs_dir_entry *entry, uint64_t size) {
    entry->size_lo = (uint32_t)(size & 0xFFFFFFFF);
    entry->size_hi = (uint32_t)(size >> 32);
}

int pugfs_format(DISK_IMG *img, uint32_t reserved_sectors, uint16_t sectors_per_cluster) {
    if (!img || reserved_sectors < 1 || sectors_per_cluster < 1) return -1;

    int disk_sector_size = disk_img_get_sector_size(img);
    if (disk_sector_size <= 0) return -1;

    if ((uint32_t) disk_sector_size < sizeof(struct pugfs_dir_entry))
        return -1;

    uint32_t total_sectors = disk_img_get_total_sectors(img);
    // disk is way too small
    if (total_sectors < 100) return -1;

    if (sectors_per_cluster >= total_sectors) return -1;

    uint32_t entries_per_sector = (uint32_t)disk_sector_size / PUGFS_FTABLE_ENTRY_BYTES;
    if (entries_per_sector == 0) return -1;

    uint32_t ftable_sectors = 0, data_sectors = 0;
    int res = _pugfs_calculate_ftable_and_data_sizes(
        total_sectors,
        reserved_sectors,
        sectors_per_cluster,
        entries_per_sector,
        &ftable_sectors,
        &data_sectors
    );
    if (res < 0) return -2;

    PUGFS *fs = _pugfs_create(img);
    if (!fs) return -3;

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
    res = disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_header), &fs->header);
    if (res != sizeof(struct pugfs_header)) {
        res = -4;
        goto format_exit;
    }

    // write the FTable
    memset(fs->buf, 0, disk_sector_size);
    *(uint32_t*)(fs->buf) = PUGFS_FTABLE_ENTRY_RESERVED; // entry 0, not used
    *((uint32_t*)(fs->buf) + 1) = PUGFS_FTABLE_ENTRY_EOC; // entry 1, root dir

    res = disk_img_stream_seek(fs->stream, reserved_sectors, 0);
    if (res != 0) {
        res = -5;
        goto format_exit;
    }

    res = disk_img_stream_write_sectors(fs->stream, 1, fs->buf);
    if (res != 1) {
        res = -6;
        goto format_exit;
    }

    memset(fs->buf, 0, disk_sector_size);
    for (uint32_t i = 1; i < ftable_sectors; ++i) {
        res = disk_img_stream_write_sectors(fs->stream, 1, fs->buf);
        if (res != 1) {
            res = -7;
            goto format_exit;
        }
    }
    
    // write EOD to root dir (at cluster 1 from the data area)
    struct pugfs_dir_entry eod;
    memset(&eod, 0, sizeof(struct pugfs_dir_entry));
    eod.type = PUGFS_DIR_ENTRY_TYPE_EOD;

    res = disk_img_stream_seek(fs->stream, reserved_sectors + ftable_sectors, 0);
    if (res != 0) {
        res = -5;
        goto format_exit;
    }

    res = disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_dir_entry), &eod);
    if (res != sizeof(struct pugfs_dir_entry)) {
        res = -6;
        goto format_exit;
    }

    res = 0; // mark success
format_exit:
    pugfs_destroy(fs);
    return res;
}

PUGFS* pugfs_mount(DISK_IMG *img) {
    if (!img) return NULL;

    PUGFS *fs = _pugfs_create(img);
    if (!fs) return NULL;

    // read boot sector
    int res = disk_img_stream_read_sectors(fs->stream, 1, fs->buf);
    if (res != 1) {
        pugfs_destroy(fs);
        return NULL;
    }

    // copy header
    memcpy(&fs->header, fs->buf, sizeof(struct pugfs_header));

    // check header fields
    if (_pugfs_check_header(&fs->header, img) < 0) {
        pugfs_destroy(fs);
        return NULL;
    }

    // calculate and set fs params
    _pugfs_set_fs_params(fs);
    
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

int pugfs_touch(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    struct pugfs_dir_entry entry;
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res == 0) {
        // file already exists

        entry.last_access_ts = 1; // TODO: set to now

        res = _pugfs_write_dir_entry_to_data_cluster(fs, cluster_index, index_in_cluster, &entry);
        if (res < 0) return -2;

        return -10;
    }

    return _pugfs_create_new_dir_entry_absolute(fs, path, PUGFS_DIR_ENTRY_TYPE_FILE, NULL, NULL, NULL);
}

int pugfs_delete(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    struct pugfs_dir_entry entry;
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res != 0) {
        // file doesnt exist, nothing to do
        return -10;
    }

    // ensure entry is valid file
    if (entry.type != PUGFS_DIR_ENTRY_TYPE_FILE) {
        return -3; // not a file
    }

    // mark entry as free and write it back
    entry.type = PUGFS_DIR_ENTRY_TYPE_FREE;
    res = _pugfs_write_dir_entry_to_data_cluster(fs, cluster_index, index_in_cluster, &entry);
    if (res != 0) {
        return -1; // failed to update entry
    }

    // free content cluster chain
    res = _pugfs_free_cluster_chain(fs, entry.starting_cluster_index);
    if (res != 0) {
        return -2; // failed to free chain
    }

    return 0;
}

int pugfs_trunc(PUGFS *fs, PATH *path, uint64_t new_size) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    if (fs->params.bytes_per_cluster == 0 || fs->params.data_clusters == 0)
        return -1;

    struct pugfs_dir_entry entry;
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res != 0) {
        // file doesnt exist
        return -1;
    }

    // ensure entry is valid file
    if (entry.type != PUGFS_DIR_ENTRY_TYPE_FILE) {
        return -2; // not a file
    }

    uint64_t entry_size = _pugfs_get_entry_size(&entry);

    // ensure file size is larger than new size
    if (entry_size <= new_size) {
        return 1; // nothing to do
    }

    uint64_t new_file_clusters = new_size / fs->params.bytes_per_cluster;
    if (new_size % fs->params.bytes_per_cluster != 0) {  // round up to next cluster
        ++new_file_clusters;
    }

    // files should have at least 1 allocated cluster
    if (new_file_clusters == 0) {
        new_file_clusters = 1;
    }

    uint32_t cluster = entry.starting_cluster_index;
    if (cluster < 1 || cluster > fs->params.data_clusters)
        return -3;

    for (uint32_t i = 1; i < new_file_clusters; ++i) {
        cluster = _pugfs_read_ftable_entry(fs, cluster);

        if (cluster < 1 || cluster > fs->params.data_clusters)
            return -3;
    }

    uint32_t next_cluster = _pugfs_read_ftable_entry(fs, cluster);
    if (next_cluster != PUGFS_FTABLE_ENTRY_EOC &&
        (next_cluster < 1 || next_cluster > fs->params.data_clusters))
        return -3;

    // terminate retained chain
    res = _pugfs_write_ftable_entry(fs, cluster, PUGFS_FTABLE_ENTRY_EOC);
    if (res != 0) {
        return -4; // failed to update ftable entry
    }

    // update logical entry size and write it back
    _pugfs_set_entry_size(&entry, new_size);
    res = _pugfs_write_dir_entry_to_data_cluster(fs, cluster_index, index_in_cluster, &entry);
    if (res != 0) {
        return -6; // failed to update entry
    }

    // release remainder of chain
    if (next_cluster != PUGFS_FTABLE_ENTRY_EOC) {
        res = _pugfs_free_cluster_chain(fs, next_cluster);
        if (res != 0) {
            return -5; // failed to free cluster chain
        }
    }

    return 0;
}

int pugfs_mkdir(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    struct pugfs_dir_entry entry;
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res == 0) {
        // dir already exists, no touching!
        return -10;
    }

    return _pugfs_create_new_dir_entry_absolute(fs, path, PUGFS_DIR_ENTRY_TYPE_DIR, NULL, NULL, NULL);
}

int pugfs_rmdir(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    // do not remove the root dir (unless?? jk)
    PATH_TOKEN *tok = path_get_first_token(path);
    if (!tok) // path is empty
        return -1;

    struct pugfs_dir_entry entry;
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res != 0) {
        // dir doesnt exist, nothing to do
        return -10;
    }

    // ensure entry is valid dir
    if (entry.type != PUGFS_DIR_ENTRY_TYPE_DIR) {
        return -3; // not a dir
    }

    // enusre dir is empty
    res = _pugfs_dir_is_empty(fs, &entry);
    if (res <= 0) {
        return -4; // dir not empty
    }

    // mark entry as free and write it back
    entry.type = PUGFS_DIR_ENTRY_TYPE_FREE;
    res = _pugfs_write_dir_entry_to_data_cluster(fs, cluster_index, index_in_cluster, &entry);
    if (res != 0) {
        return -1; // failed to update entry
    }

    // free content cluster chain
    res = _pugfs_free_cluster_chain(fs, entry.starting_cluster_index);
    if (res != 0) {
        return -2; // failed to free chain
    }

    return 0;
}

int pugfs_copy(PUGFS *fs, PATH *src, PATH *dst, int overwrite) {
    if (!fs || !src || !dst) return -1;
    if (!_pugfs_path_is_valid(fs, src) || !_pugfs_path_is_valid(fs, dst))
        return -1;

    // check if paths are equal
    if (path_equals(src, dst) == 1) {
        return 0;
    }

    // check if dst is child of src
    if (path_is_child_of(dst, src) == 1) {
        return -9;
    }

    struct pugfs_dir_entry src_entry;
    uint32_t src_index_in_cluster = 0, src_cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, src, &src_entry, &src_index_in_cluster, &src_cluster_index);
    if (res != 0) {
        return -1;
    }

    struct pugfs_dir_entry dst_entry;
    uint32_t dst_index_in_cluster = 0, dst_cluster_index = 0;
    res = _pugfs_find_dir_entry_absolute(fs, dst, &dst_entry, &dst_index_in_cluster, &dst_cluster_index);
    if (res == 0) {
        if (!overwrite) {
            return -2; // dst already exists
        }
        // will replace dest

        // check matching types
        if (src_entry.type != dst_entry.type) {
            return -8; // type mismatch
        }

        // if dir, check if it is empty
        if (dst_entry.type == PUGFS_DIR_ENTRY_TYPE_DIR) {
            res = _pugfs_dir_is_empty(fs, &dst_entry);
            if (res <= 0) {
                return -7; // dir not empty
            }
        }
    } else {
        // create new entry at dest with the same type as src (so it preallocates a cluster)
        res = _pugfs_create_new_dir_entry_absolute(fs, dst, src_entry.type, &dst_entry, &dst_index_in_cluster, &dst_cluster_index);
        if (res != 0) {
            return -3; // failed to create new entry
        }
    }
    // by this point, dst_entry and indices are valid (I hope)

    // save cluster chain start of dest if it is a file, to free later
    // if it is a dir, it does not need to be freed
    int32_t old_dest_chain_start = 0;
    if (dst_entry.type == PUGFS_DIR_ENTRY_TYPE_FILE) {
        old_dest_chain_start = dst_entry.starting_cluster_index;
    }

    // copy the src entry fields
    dst_entry.attributes = src_entry.attributes;
    dst_entry.creation_ts = src_entry.creation_ts;
    memcpy(dst_entry.reserved, src_entry.reserved, sizeof(dst_entry.reserved));
    dst_entry.size_lo = src_entry.size_lo;
    dst_entry.size_hi = src_entry.size_hi;
    dst_entry.last_access_ts = 1; // TODO: set to now

    // if copying a file, it's easy
    if (dst_entry.type == PUGFS_DIR_ENTRY_TYPE_FILE) {
        // just duplicate the file contents and update the new entry
        uint32_t new_dest_chain_start = 0;
        res = _pugfs_duplicate_cluster_chain(fs, src_entry.starting_cluster_index, &new_dest_chain_start);
        if (res != 0) {
            return -10;
        }
        dst_entry.starting_cluster_index = new_dest_chain_start;
        // all that's left is to write the dir entry to disk
    } else { // otherwise, it sucks for me 
        // pass it over to the recursive function
        res = _pugfs_copy_dir_contents_rec(fs, &src_entry, &dst_entry);
        if (res != 0) {
            return -11;
        }
    }

    // write the dst entry back to disk
    res = _pugfs_write_dir_entry_to_data_cluster(fs, dst_cluster_index, dst_index_in_cluster, &dst_entry);
    if (res != 0) {
        return -4; // failed to write dst entry
    }

    // free old cluster chain of dest if it was a file
    // otherwise it's set to 0
    if (old_dest_chain_start != 0) {
        res = _pugfs_free_cluster_chain(fs, old_dest_chain_start);
        if (res != 0) {
            return -6;
        }
    }

    return 0;
}

int pugfs_move(PUGFS *fs, PATH *src, PATH *dst, int overwrite) {
    if (!fs || !src || !dst) return -1;
    if (!_pugfs_path_is_valid(fs, src) || !_pugfs_path_is_valid(fs, dst))
        return -1;

    // check if paths are equal
    if (path_equals(src, dst) == 1) {
        return 0;
    }

    // check if dst is child of src
    if (path_is_child_of(dst, src) == 1) {
        return -9;
    }

    struct pugfs_dir_entry src_entry;
    uint32_t src_index_in_cluster = 0, src_cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, src, &src_entry, &src_index_in_cluster, &src_cluster_index);
    if (res != 0) {
        return -1;
    }

    struct pugfs_dir_entry dst_entry;
    uint32_t dst_index_in_cluster = 0, dst_cluster_index = 0;
    res = _pugfs_find_dir_entry_absolute(fs, dst, &dst_entry, &dst_index_in_cluster, &dst_cluster_index);
    if (res == 0) {
        if (!overwrite) {
            return -2; // dst already exists
        }
        // will replace dest

        // check matching types
        if (src_entry.type != dst_entry.type) {
            return -8; // type mismatch
        }

        // if dir, check if it is empty
        if (dst_entry.type == PUGFS_DIR_ENTRY_TYPE_DIR) {
            res = _pugfs_dir_is_empty(fs, &dst_entry);
            if (res <= 0) {
                return -7; // dir not empty
            }
        }
    } else {
        // create new free entry at dest (no cluster will be allocated for data)
        res = _pugfs_create_new_dir_entry_absolute(fs, dst, PUGFS_DIR_ENTRY_TYPE_FREE, &dst_entry, &dst_index_in_cluster, &dst_cluster_index);
        if (res != 0) {
            return -3; // failed to create new entry
        }
    }
    // by this point, dst_entry and indices are valid

    // save cluster chain start of dest (will be 0 if its a new entry)
    uint32_t old_dest_chain_start = dst_entry.starting_cluster_index;

    // save the name of the dst entry
    char dst_name[PUGFS_FILENAME_BYTES];
    memcpy(dst_name, dst_entry.name, PUGFS_FILENAME_BYTES);
    // copy the src entry to dst entry
    memcpy(&dst_entry, &src_entry, sizeof(struct pugfs_dir_entry));
    // overwrite the name
    memcpy(dst_entry.name, dst_name, PUGFS_FILENAME_BYTES);
    dst_entry.last_access_ts = 1; // TODO: set to now

    // write the dst entry back to disk
    res = _pugfs_write_dir_entry_to_data_cluster(fs, dst_cluster_index, dst_index_in_cluster, &dst_entry);
    if (res != 0) {
        return -4; // failed to write dst entry
    }
    
    // mark the src entry as free and write it back
    src_entry.type = PUGFS_DIR_ENTRY_TYPE_FREE;
    res = _pugfs_write_dir_entry_to_data_cluster(fs, src_cluster_index, src_index_in_cluster, &src_entry);
    if (res != 0) {
        return -5; // failed to write src entry
    }

    // free old cluster chain of dest if not newly created
    if (old_dest_chain_start != 0) {
        res = _pugfs_free_cluster_chain(fs, old_dest_chain_start);
        if (res != 0) {
            return -6;
        }
    }
    
    return 0;
}

int pugfs_lsdir_begin(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    struct pugfs_dir_entry entry;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, NULL, NULL);
    if (res != 0) { // dir not found
        return -2;
    }

    if (entry.type != PUGFS_DIR_ENTRY_TYPE_DIR) {
        return -3; // not a dir
    }

    res = _pugfs_iter_begin(&fs->lsdir, &entry);
    if (res != 0) {
        return -4; // iter error
    }

    return 0;
}

int pugfs_lsdir_next(PUGFS *fs, struct pugfs_filestat *stat_out) {
    if (!fs) return -1;
    
    int res = 0;
    struct pugfs_dir_entry *entry = NULL;
    do {
        res = _pugfs_iter_next(fs, &fs->lsdir);
        if (res <= 0) { // EOD or error
            // set ITER END flag to prevent further calls
            _pugfs_iter_end(&fs->lsdir);
            return res;
        }
        entry = &fs->lsdir.current;
        // skip over entries which are neither files nor dirs
    } while (entry != NULL && entry->type != PUGFS_DIR_ENTRY_TYPE_FILE && entry->type != PUGFS_DIR_ENTRY_TYPE_DIR);

    // write current entry stats if requested
    if (stat_out) {
        res = _pugfs_get_dir_entry_stats(entry, stat_out);
        if (res != 0) 
            return -2;
    }

    return 1;
}

pugfs_fd pugfs_fopen(PUGFS *fs, PATH *path, pugfs_file_mode_t mode) {
    if (!fs || !path) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    struct pugfs_dir_entry entry;
    uint32_t cluster_index = 0, index_in_cluster = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res != 0) return res;

    if (entry.type != PUGFS_DIR_ENTRY_TYPE_FILE)
        return -2; // can only open files

    return _pugfs_alloc_fd(fs, &entry, mode, cluster_index, index_in_cluster);
}

int pugfs_fclose(PUGFS *fs, pugfs_fd fd) {
    if (!fs || fd < 1 || fd > PUGFS_MAX_FILE_DESCRIPTORS) return -1;
    return _pugfs_free_fd(fs, fd);
}

int pugfs_fstat(PUGFS *fs, PATH *path, struct pugfs_filestat *stat_out) {
    if (!fs || !path || !stat_out) return -1;
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    struct pugfs_dir_entry entry;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, NULL, NULL);
    if (res != 0) return res;

    if (entry.type != PUGFS_DIR_ENTRY_TYPE_FILE && entry.type != PUGFS_DIR_ENTRY_TYPE_DIR)
        return -2; // entry type does not support stats

    return _pugfs_get_dir_entry_stats(&entry, stat_out);
}

int pugfs_fseek(PUGFS *fs, pugfs_fd fd, int offset, pugfs_seek_mode_t whence) {
    if (!fs || fd < 1 || fd > PUGFS_MAX_FILE_DESCRIPTORS)
        return -1;

    struct pugfs_file_descriptor *desc = &fs->file_descriptors[(int)fd - 1];
    if (desc->fd != fd) return -2;

    uint64_t entry_size = _pugfs_get_entry_size(&desc->entry);
    uint64_t new_pos = 0;

    switch (whence) {
        case PUGFS_SEEK_SET:
            new_pos = offset;
            break;

        case PUGFS_SEEK_CUR:
            new_pos = desc->byte_pos + offset;
            break;

        case PUGFS_SEEK_END:
            new_pos = entry_size + offset;
            break;

        default:
            return -1;
    }

    if (new_pos > entry_size)
        return -3;

    desc->byte_pos = new_pos;
    return 0;
}

size_t pugfs_fread(PUGFS *fs, void *dst, size_t size, size_t nmemb, pugfs_fd fd) {
    if (!fs || !dst || fd < 1 || fd > PUGFS_MAX_FILE_DESCRIPTORS)
        return 0;

    struct pugfs_file_descriptor *desc = &fs->file_descriptors[(int)fd - 1];
    if (desc->fd != fd) return 0;

    if (desc->mode != PUGFS_FILE_MODE_READ && desc->mode != PUGFS_FILE_MODE_READWRITE)
        return 0; // not allowed
    
    if (size == 0 || nmemb == 0)
        return 0; // nothing to read

    if (nmemb > SIZE_MAX / size)
        return 0;

    if (size > UINT32_MAX || nmemb > UINT32_MAX)
        return 0;

    uint64_t file_size = _pugfs_get_entry_size(&desc->entry);
    if (desc->byte_pos >= file_size)
        return 0;

    size_t total_bytes = size * nmemb;
    uint64_t remaining = file_size - desc->byte_pos;
    if (total_bytes > remaining) {
        total_bytes = remaining;
    }

    size_t blocks_to_read = total_bytes / size;
    if (blocks_to_read == 0)
        return 0;


    uint32_t blocks_read = 0;
    int res = _pugfs_read_blocks_from_cluster_chain(
        fs, 
        desc->entry.starting_cluster_index, 
        desc->byte_pos, 
        size,
        blocks_to_read,
        dst,
        &blocks_read
    );

    desc->byte_pos += blocks_read * size;

    if (res != 0)
        return blocks_read;

    return blocks_read;
}

size_t pugfs_fwrite(PUGFS *fs, const void *src, size_t size, size_t nmemb, pugfs_fd fd) {
    if (!fs || !src || fd < 1 || fd > PUGFS_MAX_FILE_DESCRIPTORS)
        return -1;

    struct pugfs_file_descriptor *desc = &fs->file_descriptors[(int)fd - 1];
    if (desc->fd != fd) return 0;

    if (desc->mode != PUGFS_FILE_MODE_WRITE && desc->mode != PUGFS_FILE_MODE_READWRITE
        && desc->mode != PUGFS_FILE_MODE_APPEND)
        return 0; // not allowed
    
    if (size == 0 || nmemb == 0)
        return 0; // nothing to write

    if (nmemb > SIZE_MAX / size)
        return 0;

    if (size > UINT32_MAX || nmemb > UINT32_MAX)
        return 0;

    size_t total_bytes = size * nmemb;
    uint64_t write_pos = desc->byte_pos;

    if (desc->mode == PUGFS_FILE_MODE_APPEND)
        write_pos = _pugfs_get_entry_size(&desc->entry);

    // check end-position overflow
    if (write_pos > UINT64_MAX - total_bytes)
        return 0;

    uint32_t blocks_written = 0;
    int res = _pugfs_write_blocks_to_cluster_chain(
        fs,
        desc->entry.starting_cluster_index,
        write_pos,
        size,
        nmemb,
        src,
        &blocks_written
    );

    uint64_t bytes_written = (uint64_t)blocks_written * size;
    uint64_t new_end = write_pos + bytes_written;

    // update logical file size if the file grew
    uint64_t old_size = _pugfs_get_entry_size(&desc->entry);
    if (new_end > old_size) {
        _pugfs_set_entry_size(&desc->entry, new_end);
        res = _pugfs_write_dir_entry_to_data_cluster(
            fs,
            desc->cluster_index,
            desc->index_in_cluster,
            &desc->entry
        );
        if (res < 0)
            return 0;
    }

    desc->byte_pos = new_end;

    if (res != 0)
        return blocks_written;

    return blocks_written;
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

    return -1;
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

    // set ITER END flag in case lsdir_next is called before lsdir_begin
    _pugfs_iter_end(&fs->lsdir);

    fs->drive_no = disk_img_get_drive_no(img);
    if (fs->drive_no < 0) {
        free(fs);
        return NULL;
    }

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

static void _pugfs_set_fs_params(PUGFS *fs) {
    if (!fs) return;
    fs->params.data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    fs->params.entries_per_sector = fs->header.sector_size / PUGFS_FTABLE_ENTRY_BYTES;
    fs->params.bytes_per_cluster = (uint64_t)fs->header.sectors_per_cluster * fs->header.sector_size;
    fs->params.entries_per_cluster = fs->params.bytes_per_cluster / sizeof(struct pugfs_dir_entry);
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
    uint32_t checksum = _pugfs_header_checksum(header);
    if (checksum != header->checksum) return -2;

    return 0; // header ok
}

static uint32_t _pugfs_read_ftable_entry(PUGFS *fs, uint32_t index) {
    if (!fs || index > PUGFS_FTABLE_ENTRY_CLUSTER_MAX) return PUGFS_FTABLE_ENTRY_INVALID;
    if (index > fs->params.data_clusters) return PUGFS_FTABLE_ENTRY_INVALID;
    
    uint32_t sector = fs->header.ftable_start_sector + index / fs->params.entries_per_sector;
    uint32_t offset = (index % fs->params.entries_per_sector) * PUGFS_FTABLE_ENTRY_BYTES;
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return PUGFS_FTABLE_ENTRY_INVALID;

    uint32_t entry = PUGFS_FTABLE_ENTRY_INVALID;
    res = disk_img_stream_read_bytes(fs->stream, PUGFS_FTABLE_ENTRY_BYTES, &entry);
    if (res != PUGFS_FTABLE_ENTRY_BYTES) return PUGFS_FTABLE_ENTRY_INVALID;
    
    return entry; 
}

static int _pugfs_write_ftable_entry(PUGFS *fs, uint32_t index, uint32_t value) {
    if (!fs || index > PUGFS_FTABLE_ENTRY_CLUSTER_MAX) return -1;
    if (index > fs->params.data_clusters) return -1;
    
    uint32_t sector = fs->header.ftable_start_sector + index / fs->params.entries_per_sector;
    uint32_t offset = (index % fs->params.entries_per_sector) * PUGFS_FTABLE_ENTRY_BYTES;
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return -1;

    res = disk_img_stream_write_bytes(fs->stream, PUGFS_FTABLE_ENTRY_BYTES, &value);
    if (res != PUGFS_FTABLE_ENTRY_BYTES) return -1;
    
    return 0;
}

static int _pugfs_allocate_cluster(PUGFS *fs, uint32_t *cluster_out) {
    if (!fs || !cluster_out) return -1;

    uint32_t total_entries = fs->params.entries_per_sector * fs->header.ftable_sectors;
    uint32_t sector = fs->header.ftable_start_sector;
    uint32_t offset = 0;
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return -1;

    // find the first free FTable entry
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

static int _pugfs_free_cluster_chain(PUGFS *fs, uint32_t starting_cluster) {
    if (!fs || !fs->stream || starting_cluster == 0) return -1;
    if (fs->header.sectors_per_cluster == 0) return -1;

    uint32_t cluster = starting_cluster;

    // the iteration limit protects against circular/corrupt chains
    for (uint32_t i = 0; i < fs->params.data_clusters; ++i) {
        if (cluster < 1 || cluster > fs->params.data_clusters)
            return -1;

        uint32_t next = _pugfs_read_ftable_entry(fs, cluster);

        if (next == PUGFS_FTABLE_ENTRY_INVALID ||
            next == PUGFS_FTABLE_ENTRY_FREE ||
            next == PUGFS_FTABLE_ENTRY_RESERVED ||
            next == PUGFS_FTABLE_ENTRY_BAD) {
            return -1;
        }

        int res = _pugfs_write_ftable_entry(fs, cluster, PUGFS_FTABLE_ENTRY_FREE);

        if (res != 0) return -1;

        if (next == PUGFS_FTABLE_ENTRY_EOC) return 0;

        if (next < 1 || next > fs->params.data_clusters) return -1;

        cluster = next;
    }

    // reaching this point means the chain is cyclic or invalid
    return -1;
}

static int _pugfs_copy_cluster_data(PUGFS *fs, uint32_t src_cluster, uint32_t dst_cluster) {
    if (!fs || !fs->stream || src_cluster == 0 || dst_cluster == 0)
        return -1;

    // check if clusters are in data area bounds
    if (src_cluster < 1 || src_cluster > fs->params.data_clusters) return -1;
    if (dst_cluster < 1 || dst_cluster > fs->params.data_clusters) return -1;

    // copy 1 sector at a time
    uint32_t sectors_per_cluster = fs->header.sectors_per_cluster;
    uint32_t src_start_sector = fs->header.data_start_sector + (src_cluster - 1) * fs->header.sectors_per_cluster;
    uint32_t dst_start_sector = fs->header.data_start_sector + (dst_cluster - 1) * fs->header.sectors_per_cluster;
    int res = -1;
    for (uint32_t i = 0; i < sectors_per_cluster; ++i) {
        // set stream to the src sector
        res = disk_img_stream_seek(fs->stream, src_start_sector + i, 0);
        if (res < 0) return -1;

        // read sector
        res = disk_img_stream_read_sectors(fs->stream, 1, fs->buf);
        if (res != 1) return -1;

        // set stream to the dst sector
        res = disk_img_stream_seek(fs->stream, dst_start_sector + i, 0);
        if (res < 0) return -1;

        // write sector
        res = disk_img_stream_write_sectors(fs->stream, 1, fs->buf);
        if (res != 1) return -1;
    }

    return 0;
}

static int _pugfs_duplicate_cluster_chain(PUGFS *fs, uint32_t src_start_cluster, uint32_t *dst_start_cluster_out) {
    if (!fs || !fs->stream || src_start_cluster == 0 || !dst_start_cluster_out)
        return -1;

    if (fs->header.sectors_per_cluster == 0)
        return -1;

    if (fs->params.data_clusters == 0)
        return -1;

    uint32_t src_cluster = src_start_cluster;
    uint32_t first_dst_cluster = 0;
    uint32_t prev_dst_cluster = 0;

    for (uint32_t i = 0; i < fs->params.data_clusters; ++i) {
        if (src_cluster < 1 || src_cluster > fs->params.data_clusters)
            goto rollback;

        uint32_t next = _pugfs_read_ftable_entry(fs, src_cluster);

        if (next == PUGFS_FTABLE_ENTRY_INVALID ||
            next == PUGFS_FTABLE_ENTRY_FREE ||
            next == PUGFS_FTABLE_ENTRY_RESERVED ||
            next == PUGFS_FTABLE_ENTRY_BAD)
            goto rollback;

        if (next != PUGFS_FTABLE_ENTRY_EOC && (next < 1 || next > fs->params.data_clusters))
            goto rollback;

        uint32_t curr_dst_cluster = 0;

        // allocate new cluster
        int res = _pugfs_allocate_cluster(fs, &curr_dst_cluster);
        if (res != 0)
            goto rollback;

        // copy cluster data
        res = _pugfs_copy_cluster_data(fs, src_cluster, curr_dst_cluster);
        if (res != 0) {
            _pugfs_write_ftable_entry(fs, curr_dst_cluster, PUGFS_FTABLE_ENTRY_FREE);
            goto rollback;
        }
    
        // link new cluster
        if (i == 0) {
            first_dst_cluster = curr_dst_cluster;
        } else {
            res = _pugfs_write_ftable_entry(fs, prev_dst_cluster, curr_dst_cluster);
            if (res != 0) {
                _pugfs_write_ftable_entry(fs, curr_dst_cluster, PUGFS_FTABLE_ENTRY_FREE);
                goto rollback;
            }
        }

        prev_dst_cluster = curr_dst_cluster;

        // end of src chain
        if (next == PUGFS_FTABLE_ENTRY_EOC) {
            *dst_start_cluster_out = first_dst_cluster;
            return 0;
        }

        src_cluster = next;
    }

    // chain is cyclic or excessively long
    goto rollback;

rollback:
    if (first_dst_cluster != 0) {
        _pugfs_free_cluster_chain(fs, first_dst_cluster);
    }
    return -1;
}

static int _pugfs_get_next_cluster_in_chain(PUGFS *fs, uint32_t cluster, uint32_t *next_cluster_out) {
    if (!fs) return -1;

    if (fs->header.sectors_per_cluster == 0 || fs->header.sector_size == 0 || fs->params.data_clusters == 0)
        return -1;  // bad fs

    if (cluster < 1 || cluster > fs->params.data_clusters)
        return -1;  // out of bounds cluster

    uint32_t ftable_entry = _pugfs_read_ftable_entry(fs, cluster);

    if (ftable_entry == PUGFS_FTABLE_ENTRY_RESERVED ||
        ftable_entry == PUGFS_FTABLE_ENTRY_INVALID ||
        ftable_entry == PUGFS_FTABLE_ENTRY_BAD ||
        ftable_entry == PUGFS_FTABLE_ENTRY_FREE)
        return -1;  // bad FTABLE
    
    if (ftable_entry == PUGFS_FTABLE_ENTRY_EOC)
        return 0; // reached end of chain

    if (ftable_entry < 1 || ftable_entry > fs->params.data_clusters)
        return -1; // next cluster out of bounds

    if (next_cluster_out) {
        *next_cluster_out = ftable_entry;
    }

    return 1; // there is a next cluster
}

static int _pugfs_get_cluster_in_chain_at_byte_offset(
    PUGFS *fs, 
    uint32_t starting_cluster,
    uint64_t byte_offset, 
    uint32_t *cluster_out,
    uint32_t *prev_cluster_out
) {
    if (!fs || !cluster_out) return -1;

    if (fs->params.bytes_per_cluster == 0 || fs->params.data_clusters == 0)
        return -1;

    if (starting_cluster < 1 || starting_cluster > fs->params.data_clusters)
        return -1;

    if (prev_cluster_out) {
        *prev_cluster_out = 0;
    }

    // traverse the chain to the cluster containing byte_offset
    uint32_t cluster = starting_cluster;
    uint64_t cluster_offset = byte_offset / fs->params.bytes_per_cluster;
    for (uint64_t i = 0; i < cluster_offset; ++i) {
        uint32_t next_cluster = 0;

        if (prev_cluster_out) {
            *prev_cluster_out = cluster;
        }

        int res = _pugfs_get_next_cluster_in_chain(fs, cluster, &next_cluster);
        if (res < 0)
            return -1;

        if (res == 0) { // EOC
            // the requested position is beyond the chain or exactly at its end
            if (byte_offset == (i + 1) * (uint64_t)fs->params.bytes_per_cluster)
                return 1;
            return -1;
        }

        cluster = next_cluster;
    }

    *cluster_out = cluster;
    return 0;
}

static int _pugfs_compare_dir_entries_by_name(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2) {
    if (!e1 || !e2) return -1;
    if (e1->type == PUGFS_DIR_ENTRY_TYPE_FREE) return -1;
    return strncmp((const char*)e1->name, (const char*)e2->name, PUGFS_FILENAME_BYTES);
}

static int _pugfs_compare_dir_entries_by_type(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2) {
    if (!e1 || !e2) return -1;
    return e1->type - e2->type;
}

static int _pugfs_compare_dir_entries_file_or_dir(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry*) {
    if (!e1) return -1;
    return (e1->type - PUGFS_DIR_ENTRY_TYPE_FILE) * (e1->type - PUGFS_DIR_ENTRY_TYPE_DIR);
}

static int _pugfs_find_dir_entry_in_dir(
    PUGFS *fs,
    const struct pugfs_de_comparator *comparator,
    const struct pugfs_dir_entry *dir,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
) {
    if (!fs || !comparator || !comparator->cmp_func || !dir) return -1;
    if (dir->type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    // prepare iterator
    struct pugfs_dir_iterator iter;
    _pugfs_iter_begin(&iter, dir);
    
    // iterate until EOD or error
    int res = 0;
    while ((res = _pugfs_iter_next(fs, &iter)) >= 0) {
        if (comparator->cmp_func(&iter.current, &comparator->cmp_entry) == 0) {
            if (out_entry) {
                memcpy(out_entry, &iter.current, sizeof(*out_entry));
            }
            if (out_index_in_cluster) {
                *out_index_in_cluster = iter.current_index_in_cluster;
            }
            if (out_cluster_index) {
                *out_cluster_index = iter.current_cluster_index;
            }
            return 0;
        }
        if (res == 0) { // reached EOD no match
            return -1;
        }
    }
    return res; // error occured
}

static int _pugfs_find_dir_entry_absolute(
    PUGFS *fs, 
    PATH *path, 
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index
) {
    if (!fs || !path || !out_entry) return -1;
    if (!_pugfs_path_is_valid(fs, path)) return -1;

    const struct pugfs_dir_entry *parent = &fs->root;
    PATH_TOKEN *tok = path_get_first_token(path);
    if (!tok) { // path is empty, return root dir
        memcpy(out_entry, &fs->root, sizeof(struct pugfs_dir_entry));
        return 0;
    }

    struct pugfs_de_comparator comparator;
    comparator.cmp_func = _pugfs_compare_dir_entries_by_name;
    memset(&comparator.cmp_entry, 0, sizeof(struct pugfs_dir_entry));

    int res = 0;
    while (tok) {
        const char *name = path_get_token_text(tok);
        size_t name_len = strlen(name);
        if (name_len == 0 || name_len >= PUGFS_FILENAME_BYTES) {
            res = -1;
            break;
        }

        memset(&comparator.cmp_entry, 0, sizeof(comparator.cmp_entry));
        memcpy(comparator.cmp_entry.name, name, name_len);

        res = _pugfs_find_dir_entry_in_dir(
            fs, 
            &comparator,
            parent,
            out_entry,
            out_index_in_cluster,
            out_cluster_index
        );
        if (res != 0) break;

        parent = out_entry;
        tok = path_get_next_token(path, tok);
    }
    
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
    if (cluster_index < 1 || cluster_index > fs->params.data_clusters) return -1;

    if (index_in_cluster >= fs->params.entries_per_cluster) return -1;

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

static int _pugfs_write_new_dir_entry_to_dir(
    PUGFS *fs, 
    const struct pugfs_dir_entry *dir, 
    struct pugfs_dir_entry *entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index
) {
    if (!fs || !dir || !entry) return -1;

    // find the the first free and the EOD entry in the parent 
    struct pugfs_dir_entry free_entry, eod_entry;
    uint32_t free_index_in_cluster = 0, eod_index_in_cluster = 0, free_cluster_index = 0, eod_cluster_index = 0;
    memset(&free_entry, 0, sizeof(free_entry));
    memset(&eod_entry, 0, sizeof(eod_entry));

    // prepare comparators
    struct pugfs_de_comparator comparator_eod, comparator_free;
    comparator_eod.cmp_func = _pugfs_compare_dir_entries_by_type;
    comparator_eod.cmp_entry.type = PUGFS_DIR_ENTRY_TYPE_EOD;
    comparator_free.cmp_func = _pugfs_compare_dir_entries_by_type;
    comparator_free.cmp_entry.type = PUGFS_DIR_ENTRY_TYPE_FREE;

    int res = _pugfs_find_dir_entry_in_dir(
        fs, 
        &comparator_eod,
        dir,
        &eod_entry,
        &eod_index_in_cluster,
        &eod_cluster_index
    );
    if (res != 0) {
        return res;   // no EOD entry found, invalid dir
    }

    res = _pugfs_find_dir_entry_in_dir(
        fs, 
        &comparator_free,
        dir,
        &free_entry,
        &free_index_in_cluster,
        &free_cluster_index
    );
    if (res != 0) { // no free entry, will replace the EOD entry
        free_index_in_cluster = eod_index_in_cluster;
        free_cluster_index = eod_cluster_index;
    
        // find space for a new EOD entry
        if (eod_index_in_cluster + 1 < fs->params.entries_per_cluster) {
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

    // allocate a cluster for the new dir entry, unless its a free entry
    uint32_t new_entry_cluster_index = 0;
    if (entry->type != PUGFS_DIR_ENTRY_TYPE_FREE) {
        res = _pugfs_allocate_cluster(fs, &new_entry_cluster_index);
        if (res != 0) return res;
        entry->starting_cluster_index = new_entry_cluster_index;
    }

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

    if (out_index_in_cluster) *out_index_in_cluster = free_index_in_cluster;
    if (out_cluster_index) *out_cluster_index = free_cluster_index;

    return 0;
}

static int _pugfs_create_new_dir_entry_absolute(
    PUGFS *fs, 
    PATH *path, 
    uint8_t type, 
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index
) {
    if (!_pugfs_path_is_valid(fs, path))
        return -1;

    // get the parent path
    PATH *parent = path_get_parent_path(path);
    if (!parent) return -1;

    // get the entry name
    char new_entry_name[PATH_MAX_TOKEN_LEN + 1] = {0};
    PATH_TOKEN *last = path_get_last_token(path);
    path_copy_token_text(last, new_entry_name);

    // prepare the new entry
    struct pugfs_dir_entry new_entry;
    memset(&new_entry, 0, sizeof(struct pugfs_dir_entry));
    new_entry.type = type;
    new_entry.size_lo = 0;
    new_entry.size_hi = 0;
    new_entry.creation_ts = 0; // TODO: set to current timestamp
    new_entry.last_access_ts = 0; // TODO: set to current timestamp
    new_entry.starting_cluster_index = 0; // will be allocated later
    new_entry.attributes = 0; // no special attributes
    strncpy((char*)new_entry.name, new_entry_name, PUGFS_FILENAME_BYTES - 1);

    // find the parent dir
    struct pugfs_dir_entry parent_dir_entry;
    memset(&parent_dir_entry, 0, sizeof(struct pugfs_dir_entry));
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, parent, &parent_dir_entry, &index_in_cluster, &cluster_index);
    path_destroy(parent);
    if (res != 0 || parent_dir_entry.type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    res = _pugfs_write_new_dir_entry_to_dir(fs, &parent_dir_entry, &new_entry, out_index_in_cluster, out_cluster_index);
    if (res != 0) {
        return res;
    }

    // propagate the new entry to the caller
    if (out_entry) {
        memcpy(out_entry, &new_entry, sizeof(struct pugfs_dir_entry));
    }

    return 0;
}

int _pugfs_rw_bytes_at_cluster(
    PUGFS *fs, 
    uint32_t cluster, 
    uint32_t offset, 
    uint32_t count, 
    void *iobuf, 
    uint8_t mode,
    uint32_t *count_out
) {
    if (!fs || !iobuf || !fs->stream) return -1;
    
    if (fs->header.sector_size == 0 ||
        fs->header.sectors_per_cluster == 0 ||
        fs->params.bytes_per_cluster == 0 ||
        fs->params.data_clusters == 0)
        return -1; // bad fs

    if (cluster < 1 || cluster > fs->params.data_clusters)
        return -1;  // out of bounds cluster

    if (count == 0) return 0; // nothing to read

    if (offset > fs->params.bytes_per_cluster || count > fs->params.bytes_per_cluster - offset)
        return -1;

    if (count > INT32_MAX)
        return -1;

    // set stream position to the first sector of the cluster
    uint64_t sector = (uint64_t)fs->header.data_start_sector + (uint64_t)(cluster - 1) * fs->header.sectors_per_cluster;

    if (sector > UINT32_MAX)
        return -1;

    int res = disk_img_stream_seek(fs->stream, sector, 0);
    if (res < 0) return -1; // disk seek error
    
    // advance stream to offset across sector boundaries
    disk_img_stream_advance(fs->stream, offset);

    // read or write 'count' bytes
    if (mode == PUGFS_RW_AT_CLUSTER_MODE_READ) {
        res = disk_img_stream_read_bytes(fs->stream, count, iobuf);
    } else if (mode == PUGFS_RW_AT_CLUSTER_MODE_WRITE) {
        res = disk_img_stream_write_bytes(fs->stream, count, iobuf);
    } else {
        return -1; // invalid mode
    }

    if (res < 0) {
        return -1; // read/write error
    }

    if (count_out) {
        *count_out = (uint32_t)res;
    }
    
    if ((uint32_t)res != count)
        return -2; // less bytes than requested

    return 0;
}

int _pugfs_read_blocks_from_cluster_chain(
    PUGFS *fs,
    uint32_t starting_cluster,
    uint64_t byte_offset, 
    uint32_t bs, 
    uint32_t count, 
    void *out,
    uint32_t *blocks_read
) {
    if (!fs || !out) return -1;

    if (fs->header.sector_size == 0 ||
        fs->header.sectors_per_cluster == 0 ||
        fs->params.bytes_per_cluster == 0 ||
        fs->params.data_clusters == 0)
        return -1; // bad fs

    if (blocks_read) {
        *blocks_read = 0;
    }

    if (bs == 0 || count == 0) return 0; // nothing to read

    // validate starting cluster
    if (starting_cluster < 1 || starting_cluster > fs->params.data_clusters)
        return -1;

    // find cluster containing the byte offset
    uint32_t cluster = 0;
    int res = _pugfs_get_cluster_in_chain_at_byte_offset(fs, starting_cluster, byte_offset, &cluster, NULL);
    if (res != 0)
        return -1;

    // calculate the remaining offset within the found cluster
    uint32_t offset = byte_offset % fs->params.bytes_per_cluster;

    // read 'count' blocks of size 'bs'
    uint8_t *outc = out;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bytes_remaining = bs;
    
        while (bytes_remaining > 0) {
            // calculate how many bytes can be read from this cluster
            uint32_t available = fs->params.bytes_per_cluster - offset;

            uint32_t bytes_to_read = bytes_remaining < available ? bytes_remaining : available;

            res = _pugfs_rw_bytes_at_cluster(fs, cluster, offset, bytes_to_read, outc, PUGFS_RW_AT_CLUSTER_MODE_READ, NULL);
            if (res != 0)
                return -1;

            outc += bytes_to_read;
            bytes_remaining -= bytes_to_read;
            offset += bytes_to_read;

            if (bytes_remaining > 0) {
                // move to the next cluster in the chain
                uint32_t next_cluster = 0;
                res = _pugfs_get_next_cluster_in_chain(fs, cluster, &next_cluster);
                if (res != 1)
                    return -1; // EOC or corrupt chain

                cluster = next_cluster;
                offset = 0;
            }
        }

        if (blocks_read) {
            *blocks_read = i + 1;
        }
    }

    return 0;
}

int _pugfs_write_blocks_to_cluster_chain(
    PUGFS *fs, 
    uint32_t starting_cluster,
    uint64_t byte_offset, 
    uint32_t bs, 
    uint32_t count, 
    const void *in,
    uint32_t *blocks_written
) {
    if (!fs || !in) return -1;

    if (fs->header.sector_size == 0 ||
        fs->header.sectors_per_cluster == 0 ||
        fs->params.bytes_per_cluster == 0 ||
        fs->params.data_clusters == 0)
        return -1; // bad fs

    if (blocks_written) {
        *blocks_written = 0;
    }

    if (bs == 0 || count == 0) return 0; // nothing to read

    // validate starting cluster
    if (starting_cluster < 1 || starting_cluster > fs->params.data_clusters)
        return -1;

    // find cluster containing the byte offset
    uint32_t cluster = 0, prev_cluster = 0;
    int res = _pugfs_get_cluster_in_chain_at_byte_offset(fs, starting_cluster, byte_offset, &cluster, &prev_cluster);
    if (res < 0)
        return -1;

    if (res == 1) { // offset is at EOC
        // allocate a new cluster (for append)
        res = _pugfs_allocate_cluster(fs, &cluster);
        if (res != 0) return res;

        // update the FTable entry for the previous cluster to point to the new cluster
        res = _pugfs_write_ftable_entry(fs, prev_cluster, cluster);
        if (res != 0) return res;
    }

    // calculate the remaining offset within the found cluster
    uint32_t offset = byte_offset % fs->params.bytes_per_cluster;

    // write 'count' blocks of size 'bs'
    const uint8_t *inc = in;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bytes_remaining = bs;

        while (bytes_remaining > 0) {
            // calculate how many bytes can be read from this cluster
            uint32_t available = fs->params.bytes_per_cluster - offset;

            uint32_t bytes_to_write = bytes_remaining < available ? bytes_remaining : available;

            res = _pugfs_rw_bytes_at_cluster(fs, cluster, offset, bytes_to_write, (void*)inc, PUGFS_RW_AT_CLUSTER_MODE_WRITE, NULL);
            if (res != 0)
                return -1;

            inc += bytes_to_write;
            bytes_remaining -= bytes_to_write;
            offset += bytes_to_write;

            if (bytes_remaining > 0) {
                // move to the next cluster in the chain
                uint32_t next_cluster = 0;
                res = _pugfs_get_next_cluster_in_chain(fs, cluster, &next_cluster);
                if (res < 0)
                    return -1; // error or corrupt chain

                if (res == 0) { // EOC
                    // need to allocate a new cluster
                    res = _pugfs_allocate_cluster(fs, &next_cluster);
                    if (res != 0) return res;

                    // update the FTable entry for the current cluster to point to the new cluster
                    res = _pugfs_write_ftable_entry(fs, cluster, next_cluster);
                    if (res != 0) return res;
                }

                cluster = next_cluster;
                offset = 0;
            }
        }

        if (blocks_written) {
            *blocks_written = i + 1;
        }
    }

    return 0;
}

static int _pugfs_path_is_valid(PUGFS *fs, PATH *path) {
    if (!fs || !path) return 0;
    return path_get_drive_number(path) == fs->drive_no;
}

static int _pugfs_get_dir_entry_stats(const struct pugfs_dir_entry *entry, struct pugfs_filestat *out) {
    if (!entry || !out) return -1;

    memset(out, 0, sizeof(*out));
    memcpy(out->name, entry->name, PUGFS_FILENAME_BYTES);
    out->creation_ts = entry->creation_ts;
    out->last_access_ts = entry->last_access_ts;
    out->attributes = entry->attributes;
    out->size = _pugfs_get_entry_size(entry);
    out->type = (entry->type == PUGFS_DIR_ENTRY_TYPE_FILE) ? PUGFS_FILE_FILE : PUGFS_FILE_DIR;

    return 0;
}

static int _pugfs_dir_is_empty(PUGFS *fs, const struct pugfs_dir_entry *dir) {
    if (!fs || !dir) return -1;

    if (dir->type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    struct pugfs_dir_entry found_entry;
    memset(&found_entry, 0, sizeof(found_entry));
    uint32_t cluster_index, index_in_cluster;

    struct pugfs_de_comparator comparator;
    comparator.cmp_func = _pugfs_compare_dir_entries_file_or_dir;
    // cmp_entry not needed

    int res = _pugfs_find_dir_entry_in_dir(
        fs,
        &comparator,
        dir,
        &found_entry,
        &index_in_cluster,
        &cluster_index
    );

    if (res == 0)
        return 0;  // found a file or subdirectory

    if (res == -1)
        return 1;  // nothing found

    return -1;     // unexpected result
}

static int _pugfs_copy_dir_contents_rec(PUGFS *fs, const struct pugfs_dir_entry *src, const struct pugfs_dir_entry *dst) {
    if (!fs || !src || !dst) {printf("invalid args\n"); return -1; }
    if (src->type != PUGFS_DIR_ENTRY_TYPE_DIR || dst->type != PUGFS_DIR_ENTRY_TYPE_DIR) {
        return -1;
    }

    // prepare iterator
    struct pugfs_dir_iterator iter;
    _pugfs_iter_begin(&iter, src);
    
    // iterate until EOD or error
    int res = 0;
    struct pugfs_dir_entry new_entry;
    uint32_t new_cluster_index = 0, new_index_in_cluster = 0;
    while ((res = _pugfs_iter_next(fs, &iter)) > 0) {
        // if entry not file or dir skip
        if (iter.current.type != PUGFS_DIR_ENTRY_TYPE_DIR && iter.current.type != PUGFS_DIR_ENTRY_TYPE_FILE) {
            continue;
        }

        // prepare new entry
        memcpy(&new_entry, &iter.current, sizeof(new_entry));
        new_entry.last_access_ts = 1; // TODO: set to now

        if (iter.current.type == PUGFS_DIR_ENTRY_TYPE_FILE) {
            // duplicate cluster chain
            uint32_t new_data_start_cluster = 0;
            res = _pugfs_duplicate_cluster_chain(fs, iter.current.starting_cluster_index, &new_data_start_cluster);
            if (res != 0) {
                return res;
            }

            // crate new entry as free
            new_entry.type = PUGFS_DIR_ENTRY_TYPE_FREE; // free so the write func won't allocate a cluster
            res = _pugfs_write_new_dir_entry_to_dir(fs, dst, &new_entry, &new_index_in_cluster, &new_cluster_index);
            if (res != 0) {
                _pugfs_free_cluster_chain(fs, new_data_start_cluster);
                return res;
            }

            // update the new entry
            new_entry.starting_cluster_index = new_data_start_cluster;
            new_entry.type = PUGFS_DIR_ENTRY_TYPE_FILE; // restore the file flag
        } else if (iter.current.type == PUGFS_DIR_ENTRY_TYPE_DIR) {
            // the write will allocate a cluster and add an EOD automatically
            res = _pugfs_write_new_dir_entry_to_dir(fs, dst, &new_entry, &new_index_in_cluster, &new_cluster_index);
            if (res != 0) {
                return res;
            }

            // (re)curse
            res = _pugfs_copy_dir_contents_rec(fs, &iter.current, &new_entry);
            if (res != 0) {
                return res;
            }
        }

        // write the final new entry back to the disk
        res = _pugfs_write_dir_entry_to_data_cluster(fs, new_cluster_index, new_index_in_cluster, &new_entry);
        if (res != 0) {
            return res;
        }
    }

    if (res < 0) 
        return res; // error encountered

    return 0;  // successful copy
}

static int _pugfs_iter_begin(struct pugfs_dir_iterator *iter, const struct pugfs_dir_entry *dir) {
    if (!iter || !dir) return -1;
    memset(iter, 0, sizeof(*iter)); // this clears PUGFS_DIR_ITERATOR_FLAG_INVALID 
    memcpy(&iter->_parent, dir, sizeof(iter->_parent));
    iter->_flags |= PUGFS_DIR_ITERATOR_FLAG_BEGIN;
    return 0;
}

static int _pugfs_iter_end(struct pugfs_dir_iterator *iter) {
    if (!iter) return -1;
    iter->_flags |= PUGFS_DIR_ITERATOR_FLAG_END; // will cause iter_next to return error
    return 0;
}

static int _pugfs_iter_next(PUGFS *fs, struct pugfs_dir_iterator *iter) {
    if (!fs || !iter || !fs->stream || (iter->_flags & PUGFS_DIR_ITERATOR_FLAG_END)) 
        return -1;  // bad args

    if (fs->header.sectors_per_cluster == 0 || fs->header.sector_size == 0)
        return -1;  // bad fs

    if (fs->params.entries_per_cluster == 0 || fs->params.data_clusters == 0)
        return -1;  // bad fs

    if (iter->_flags & PUGFS_DIR_ITERATOR_FLAG_BEGIN) { // start iteration from the beginning
        iter->_next_cluster_index = iter->_parent.starting_cluster_index;
        iter->_next_index_in_cluster = 0;
        iter->_clusters_visited = 1; // first cluster visited
        iter->_flags &= ~PUGFS_DIR_ITERATOR_FLAG_BEGIN; // next call will continue iteration
    }

    if (iter->_next_cluster_index < 1 || iter->_next_cluster_index > fs->params.data_clusters)
        return -1; // cluster oub

    uint64_t sector = (uint64_t)fs->header.data_start_sector + (uint64_t)(iter->_next_cluster_index - 1) * fs->header.sectors_per_cluster;
    memset(&iter->current, 0, sizeof(iter->current));
    int res = disk_img_stream_seek(fs->stream, sector, 0);
    if (res < 0) return -1; // disk seek error
    
    // advance stream across sector boundaries
    disk_img_stream_advance(fs->stream, iter->_next_index_in_cluster * sizeof(struct pugfs_dir_entry));

    res = disk_img_stream_read_bytes(fs->stream, sizeof(struct pugfs_dir_entry), &iter->current);
    if (res != sizeof(struct pugfs_dir_entry))
        return -1;  // disk read error

    // save entry location
    iter->current_cluster_index = iter->_next_cluster_index;
    iter->current_index_in_cluster = iter->_next_index_in_cluster;

    if (iter->current.type == PUGFS_DIR_ENTRY_TYPE_EOD)
        return 0;   // reached the end of the dir, hurray! (but pls dont call iter_next again!)

    ++iter->_next_index_in_cluster;

    if (iter->_next_index_in_cluster >= fs->params.entries_per_cluster) {
        iter->_next_index_in_cluster = 0;

        uint32_t ftable_entry = 0;
        res = _pugfs_get_next_cluster_in_chain(fs, iter->_next_cluster_index, &ftable_entry);
        if (res < 0) 
            return -1; // bad next cluster

        if (res == 0)
            return -2; // encountered EOC and directory lacks EOD

        if (iter->_clusters_visited >= fs->params.data_clusters)
            return -1; // cycle / excessively long chain

        iter->_next_cluster_index = ftable_entry;
        ++iter->_clusters_visited;
    }

    return 1; // there is a next entry
}

static int _pugfs_alloc_fd(
    PUGFS *fs, 
    const struct pugfs_dir_entry *entry, 
    pugfs_file_mode_t mode, 
    uint32_t cluster_index, 
    uint32_t index_in_cluster
) {
    if (!fs || !entry) return -1;
    struct pugfs_file_descriptor *desc = NULL;
    for (int i = 0; i < PUGFS_MAX_FILE_DESCRIPTORS; ++i) {
        desc = &fs->file_descriptors[i];
        if (desc->fd <= 0) {
            // found free descriptor
            desc->byte_pos = 0;
            desc->mode = mode;
            desc->fd = (pugfs_fd)(i + 1); // fd's start at 1
            desc->cluster_index = cluster_index;
            desc->index_in_cluster = index_in_cluster;
            memcpy(&desc->entry, entry, sizeof(desc->entry));
            return i + 1;
        }
    }

    return 0; // no free descriptors
}

static int _pugfs_free_fd(PUGFS *fs, pugfs_fd fd) {
    if (!fs || fd < 1 || fd > PUGFS_MAX_FILE_DESCRIPTORS) return -1;
    struct pugfs_file_descriptor *desc = &fs->file_descriptors[(int)fd - 1];
    if (desc->fd == fd) {
        // descriptor is valid and allocated
        desc->fd = 0;
        return 0; // success
    }
    return -1; // error
}