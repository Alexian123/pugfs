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

#define PUGFS_DIR_ITERATOR_FLAG_BEGIN   0x01

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
    int drive_no;
    struct pugfs_header header;
    struct pugfs_dir_entry root;
    DISK_IMG_STREAM *stream;
    char *buf;
};

// define public and private fields
struct pugfs_dir_iterator
{
    // public fields - access freely from outside iter_next
    const struct pugfs_dir_entry *parent;   // must not be NULL, set before calling iter_next
    struct pugfs_dir_entry current;         // copy of current entry, overwritten inside iter_next
    uint8_t flags;                          // set bit 0 to start from the beginning
    uint32_t current_cluster_index;         // cluster index of current entry (for writing back)
    uint32_t current_index_in_cluster;      // index in cluster of current entry (for writing back)

    // private fields - should not be accessed outside of iter_next
    uint32_t _clusters_visited;             // counts visited clusters to prevent infinite loops in case of corrupted chains
    uint32_t _next_cluster_index;           // cluster index for next iteration
    uint32_t _next_index_in_cluster;        // index in cluster for next iteration
};

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

// ftable access
static uint32_t _pugfs_read_ftable_entry(PUGFS *fs, uint32_t index);
static int _pugfs_write_ftable_entry(PUGFS *fs, uint32_t index, uint32_t value);

// cluster chain management
static int _pugfs_allocate_cluster(PUGFS *fs, uint32_t *cluster_out);
static int _pugfs_free_cluster_chain(PUGFS *fs, uint32_t starting_cluster);
static int _pugfs_copy_cluster_data(PUGFS *fs, uint32_t src_cluster, uint32_t dst_cluster);
static int _pugfs_duplicate_cluster_chain(PUGFS *fs, uint32_t src_start_cluster, uint32_t *dst_start_cluster_out);

// functions for comparing directory entries
static int _pugfs_compare_dir_entries_by_name(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);
static int _pugfs_compare_dir_entries_by_type(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry *e2);
static int _pugfs_compare_dir_entries_file_or_dir(const struct pugfs_dir_entry *e1, const struct pugfs_dir_entry*);

// query functions for directory entries
static int _pugfs_find_dir_entry_in_dir(
    PUGFS *fs,
    const struct pugfs_de_comparator *comparator,
    const struct pugfs_dir_entry *dir,
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
);
static int _pugfs_list_dir_entries_in_dir(
    PUGFS *fs,
    const struct pugfs_de_comparator *comparator,
    const struct pugfs_dir_entry *dir,
    char *list,
    int max_entries
);
static int _pugfs_find_dir_entry_absolute(
    PUGFS *fs, 
    PATH *path, 
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index // FTable index of the cluster containing the entry
);

// write functions for directory entries
static int _pugfs_write_dir_entry_to_data_cluster(
    PUGFS *fs,
    uint32_t cluster_index,
    uint32_t index_in_cluster,
    const struct pugfs_dir_entry *entry
);
static int _pugfs_write_new_dir_entry_to_dir(
    PUGFS *fs, 
    const struct pugfs_dir_entry *dir, 
    struct pugfs_dir_entry *entry,      // 1 cluster will be allocated if entry.type != FREE
    uint32_t *out_index_in_cluster, 
    uint32_t *out_cluster_index
);
static int _pugfs_create_new_dir_entry_absolute(
    PUGFS *fs, 
    PATH *path,
    uint8_t type,                       // 1 cluster will be allocated if type != FREE
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index
);

// utility functions
static int _pugfs_dir_is_empty(PUGFS *fs, const struct pugfs_dir_entry *dir); // 1 if empty, 0 if not, <0 on error
static int _pugfs_copy_dir_contents_rec(PUGFS *fs, const struct pugfs_dir_entry *src, const struct pugfs_dir_entry *dst);
// return 1 if has next, 0 if EOD, -1 on error, -2 on EOC (and not EOD)
static int _pugfs_iter_next(PUGFS *fs, struct pugfs_dir_iterator *iter);

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
    disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_header), &fs->header);

    // write the FTable
    memset(fs->buf, 0, disk_sector_size);
    *(uint32_t*)(fs->buf) = PUGFS_FTABLE_ENTRY_RESERVED; // entry 0, not used
    *((uint32_t*)(fs->buf) + 1) = PUGFS_FTABLE_ENTRY_EOC; // entry 1, root dir

    disk_img_stream_seek(fs->stream, reserved_sectors, 0);
    disk_img_stream_write_sectors(fs->stream, 1, fs->buf);

    memset(fs->buf, 0, disk_sector_size);
    for (uint32_t i = 1; i < ftable_sectors; ++i) {
        disk_img_stream_write_sectors(fs->stream, 1, fs->buf);
    }
    
    // write EOD to root dir (at cluster 1 from the data area)
    struct pugfs_dir_entry eod;
    memset(&eod, 0, sizeof(struct pugfs_dir_entry));
    eod.type = PUGFS_DIR_ENTRY_TYPE_EOD;
    disk_img_stream_seek(fs->stream, reserved_sectors + ftable_sectors, 0);
    disk_img_stream_write_bytes(fs->stream, sizeof(struct pugfs_dir_entry), &eod);

    pugfs_destroy(fs);
    return 0;
}

PUGFS* pugfs_mount(DISK_IMG *img) {
    if (!img) return NULL;

    PUGFS *fs = _pugfs_create(img);
    if (!fs) return NULL;

    // read boot sector
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

int pugfs_touch(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;

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

int pugfs_trunc(PUGFS *fs, PATH *path, uint32_t new_size) {
    if (!fs || !path) return -1;

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

    // ensure file size is larger than new size
    if (entry.size <= new_size) {
        return 1; // nothing to do
    }

    uint32_t bytes_per_cluster = fs->header.sector_size * fs->header.sectors_per_cluster;
    if (entry.size <= bytes_per_cluster) {
        return -10; // nothing to do, min file size is 1 cluster
    }
    uint32_t file_clusters = entry.size / bytes_per_cluster;
    if (entry.size % bytes_per_cluster != 0) {  // round up to next cluster
        ++file_clusters;
    }

    uint32_t new_file_clusters = new_size / bytes_per_cluster;
    if (new_size % bytes_per_cluster != 0) {  // round up to next cluster
        ++new_file_clusters;
    }

    uint32_t cluster = entry.starting_cluster_index;
    for (uint32_t i = 1; i < new_file_clusters; ++i) {
        cluster = _pugfs_read_ftable_entry(fs, cluster);
        if (cluster == PUGFS_FTABLE_ENTRY_EOC || cluster == PUGFS_FTABLE_ENTRY_INVALID) {
            return -3; 
        }
    }

    uint32_t next_cluster = _pugfs_read_ftable_entry(fs, cluster);

    if (next_cluster == PUGFS_FTABLE_ENTRY_INVALID ||
        next_cluster == PUGFS_FTABLE_ENTRY_FREE ||
        next_cluster == PUGFS_FTABLE_ENTRY_RESERVED ||
        next_cluster == PUGFS_FTABLE_ENTRY_BAD) {
        return -3;
    }

    // update entry size and write it back
    entry.size = new_size;
    res = _pugfs_write_dir_entry_to_data_cluster(fs, cluster_index, index_in_cluster, &entry);
    if (res != 0) {
        return -6; // failed to update entry
    }

    res = _pugfs_write_ftable_entry(fs, cluster, PUGFS_FTABLE_ENTRY_EOC);
    if (res != 0) {
        return -4; // failed to update ftable entry
    }
    res = _pugfs_free_cluster_chain(fs, next_cluster);
    if (res != 0) {
        return -5; // failed to free cluster chain
    }

    return 0;
}

int pugfs_mkdir(PUGFS *fs, PATH *path) {
    if (!fs || !path) return -1;

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

int pugfs_lsdir(PUGFS *fs, PATH *path, char *list_out, int max_entries) {
    if (!fs || !list_out || max_entries < 1) return -1;

    struct pugfs_dir_entry entry;
    uint32_t index_in_cluster = 0, cluster_index = 0;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, &index_in_cluster, &cluster_index);
    if (res != 0) { // dir not found
        return -1;
    }

    if (entry.type != PUGFS_DIR_ENTRY_TYPE_DIR) {
        return -2; // not a dir
    }

    memset(list_out, 0, max_entries * PUGFS_FILENAME_BYTES);
    struct pugfs_de_comparator comparator;
    // cmp_entry is not needed
    comparator.cmp_func = _pugfs_compare_dir_entries_file_or_dir; // will match entries of type file or dir
    return _pugfs_list_dir_entries_in_dir(fs, &comparator, &entry, list_out, max_entries);
}

int pugfs_copy(PUGFS *fs, PATH *src, PATH *dst, int overwrite) {
    if (!fs || !src || !dst) return -1;

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
    dst_entry.size = src_entry.size;
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

pugfs_fd pugfs_fopen(PUGFS *fs, PATH *path, pugfs_file_mode_t mode) {
    if (!fs || !path) return -1;

    struct pugfs_dir_entry entry;
    int res = _pugfs_find_dir_entry_absolute(fs, path, &entry, NULL, NULL);

    return -1;
}

int pugfs_fclose(PUGFS *fs, pugfs_fd fd) {
    return -1;
}

int pugfs_fstat(PUGFS *fs, PATH *path, struct pugfs_filestat *stat_out) {
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

    fs->drive_no = 0; // TODO: get from disk
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
    uint32_t checksum = _pugfs_header_checksum(header);
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

    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    uint32_t cluster = starting_cluster;

    // the iteration limit protects against circular/corrupt chains
    for (uint32_t i = 0; i < data_clusters; ++i) {
        if (cluster < 1 || cluster > data_clusters)
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

        if (next < 1 || next > data_clusters) return -1;

        cluster = next;
    }

    // reaching this point means the chain is cyclic or invalid
    return -1;
}

static int _pugfs_copy_cluster_data(PUGFS *fs, uint32_t src_cluster, uint32_t dst_cluster) {
    if (!fs || !fs->stream || src_cluster == 0 || dst_cluster == 0)
        return -1;

    // check if clusters are in data area bounds
    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    if (src_cluster < 1 || src_cluster > data_clusters) return -1;
    if (dst_cluster < 1 || dst_cluster > data_clusters) return -1;

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

    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;

    if (data_clusters == 0)
        return -1;

    uint32_t src_cluster = src_start_cluster;
    uint32_t first_dst_cluster = 0;
    uint32_t prev_dst_cluster = 0;

    for (uint32_t i = 0; i < data_clusters; ++i) {
        if (src_cluster < 1 || src_cluster > data_clusters)
            goto rollback;

        uint32_t next = _pugfs_read_ftable_entry(fs, src_cluster);

        if (next == PUGFS_FTABLE_ENTRY_INVALID ||
            next == PUGFS_FTABLE_ENTRY_FREE ||
            next == PUGFS_FTABLE_ENTRY_RESERVED ||
            next == PUGFS_FTABLE_ENTRY_BAD)
            goto rollback;

        if (next != PUGFS_FTABLE_ENTRY_EOC && (next < 1 || next > data_clusters))
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
    if (!fs || !comparator || !comparator->cmp_func || !dir || !out_index_in_cluster || !out_cluster_index) return -1;
    if (dir->type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    // prepare iterator
    struct pugfs_dir_iterator iter;
    memset(&iter, 0, sizeof(iter));
    iter.parent = dir;
    iter.flags |= PUGFS_DIR_ITERATOR_FLAG_BEGIN;
    
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

static int _pugfs_list_dir_entries_in_dir(
    PUGFS *fs,
    const struct pugfs_de_comparator *comparator,
    const struct pugfs_dir_entry *dir,
    char *list,
    int max_entries
) {
    if (!fs || !comparator || !comparator->cmp_func || !dir || !list || max_entries <= 0) return -1;
    if (dir->type != PUGFS_DIR_ENTRY_TYPE_DIR) return -1;

    // prepare iterator
    struct pugfs_dir_iterator iter;
    memset(&iter, 0, sizeof(iter));
    iter.parent = dir;
    iter.flags |= PUGFS_DIR_ITERATOR_FLAG_BEGIN;
    
    // iterate until EOD or error
    int found_entries = 0;
    int res = 0;
    while ((res = _pugfs_iter_next(fs, &iter)) >= 0) {
        if (found_entries >= max_entries) {
            break;
        }

        if (comparator->cmp_func(&iter.current, &comparator->cmp_entry) == 0) {
            memcpy(list, iter.current.name, PUGFS_FILENAME_BYTES);
            ++found_entries;
            list += PUGFS_FILENAME_BYTES;
        }

        if (res == 0) { // reached EOD
            break;
        }
    }

    if (res < 0) 
        return res; // error encountered

    return found_entries;
}

static int _pugfs_find_dir_entry_absolute(
    PUGFS *fs, 
    PATH *path, 
    struct pugfs_dir_entry *out_entry,
    uint32_t *out_index_in_cluster,
    uint32_t *out_cluster_index
) {
    if (!fs || !path || !out_entry) return -1;

    // TODO: validate drive number matches between fs and path

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
    if (fs->drive_no != path_get_drive_number(path))
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
    new_entry.size = 0;
    new_entry.creation_ts = 0; // TODO: set to current timestamp
    new_entry.last_access_ts = 0; // TODO: set to current timestamp
    new_entry.starting_cluster_index = 0; // will be allocated later
    new_entry.attributes = 0; // no special attributes
    strncpy((char*)new_entry.name, new_entry_name, PUGFS_FILENAME_BYTES - 1);
    if (type == PUGFS_DIR_ENTRY_TYPE_FILE) {
        new_entry.size = fs->header.sectors_per_cluster * fs->header.sector_size; // size of one cluster for a new file
    }

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
    memset(&iter, 0, sizeof(iter));
    iter.parent = src;
    iter.flags |= PUGFS_DIR_ITERATOR_FLAG_BEGIN;
    
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

static int _pugfs_iter_next(PUGFS *fs, struct pugfs_dir_iterator *iter) {
    if (!fs || !iter || !iter->parent || !fs->stream) 
        return -1;  // bad args

    if (fs->header.sectors_per_cluster == 0 || fs->header.sector_size == 0)
        return -1;  // bad fs

    uint32_t data_clusters = fs->header.data_sectors / fs->header.sectors_per_cluster;
    uint64_t bytes_per_cluster = (uint64_t)fs->header.sectors_per_cluster * fs->header.sector_size;
    uint32_t entries_per_cluster = bytes_per_cluster / sizeof(struct pugfs_dir_entry);

    if (entries_per_cluster == 0 || data_clusters == 0)
        return -1;  // bad fs

    if (iter->flags & PUGFS_DIR_ITERATOR_FLAG_BEGIN) { // start iteration from the beginning
        iter->_next_cluster_index = iter->parent->starting_cluster_index;
        iter->_next_index_in_cluster = 0;
        iter->_clusters_visited = 1; // first cluster visited
        iter->flags &= ~PUGFS_DIR_ITERATOR_FLAG_BEGIN; // next call will continue iteration
    }

    if (iter->_next_cluster_index < 1 || iter->_next_cluster_index > data_clusters)
        return -1;  // out of bounds cluster

    uint32_t ftable_entry = _pugfs_read_ftable_entry(fs, iter->_next_cluster_index);

    if (ftable_entry == PUGFS_FTABLE_ENTRY_RESERVED ||
        ftable_entry == PUGFS_FTABLE_ENTRY_INVALID ||
        ftable_entry == PUGFS_FTABLE_ENTRY_BAD ||
        ftable_entry == PUGFS_FTABLE_ENTRY_FREE)
        return -1;  // bad FTABLE

    uint64_t sector = (uint64_t)fs->header.data_start_sector + (uint64_t)(iter->_next_cluster_index - 1) * fs->header.sectors_per_cluster;
    uint64_t offset = (uint64_t)iter->_next_index_in_cluster * sizeof(struct pugfs_dir_entry);

    memset(&iter->current, 0, sizeof(iter->current));
    int res = disk_img_stream_seek(fs->stream, sector, offset);
    if (res < 0) return -1; // disk seek error

    res = disk_img_stream_read_bytes(fs->stream, sizeof(struct pugfs_dir_entry), &iter->current);
    if (res != sizeof(struct pugfs_dir_entry))
        return -1;  // disk read error

    // save entry location
    iter->current_cluster_index = iter->_next_cluster_index;
    iter->current_index_in_cluster = iter->_next_index_in_cluster;

    if (iter->current.type == PUGFS_DIR_ENTRY_TYPE_EOD)
        return 0;   // reached the end of the dir, hurray! (but pls dont call iter_next again!)

    ++iter->_next_index_in_cluster;

    if (iter->_next_index_in_cluster >= entries_per_cluster) {
        iter->_next_index_in_cluster = 0;

        if (ftable_entry == PUGFS_FTABLE_ENTRY_EOC)
            return -2; // directory lacks EOD

        if (ftable_entry < 1 || ftable_entry > data_clusters)
            return -1; // cluster out of bounds

        if (iter->_clusters_visited >= data_clusters)
            return -1; // cycle / excessively long chain

        iter->_next_cluster_index = ftable_entry;
        ++iter->_clusters_visited;
    }

    return 1; // there is a next entry
}