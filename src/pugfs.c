#include "pugfs.h"

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
#define PUGfS_FTABLE_ENTRY_INVALID          0xFFFFFFFF
#define PUGFS_FTABLE_ENTRY_CLUSTER_MIN      0x00000001
#define PUGFS_FTABLE_ENTRY_CLUSTER_MAX      0xFFFFFFFB

#define PUGFS_DIR_ENTRY_TYPE_EOD        0xFF    // marks the end of directory, not a real file
#define PUGFS_DIR_ENTRY_TYPE_BAD        0xFE    // bad or corrupted entry
#define PUGFS_DIR_ENTRY_TYPE_DIR        0xFD    // directory
#define PUGFS_DIR_ENTRY_TYPE_FILE       0xFC    // regular file

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
    DISK_IMG_STREAM *stream;
    char *buf;
};

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

PUGFS* pugfs_format(DISK_IMG *img, uint32_t reserved_sectors, uint16_t sectors_per_cluster) {
    if (!img || reserved_sectors < 1 || sectors_per_cluster < 1) return NULL;

    int disk_sector_size = disk_img_get_sector_size(img);
    if (disk_sector_size <= 0) return NULL;

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
    
}

int pugfs_delete(PUGFS *fs, const char *path) {

}

int pugfs_trunc(PUGFS *fs, const char *path, uint32_t new_size) {
    
}

int pugfs_mkdir(PUGFS *fs, const char *path) {

}

int pugfs_rmdir(PUGFS *fs, const char *path) {

}

int pugfs_lsdir(PUGFS *fs, const char *path, char **list_out, size_t start_index, size_t max_entries) {

}

int pugfs_copy(PUGFS *fs, const char *src, const char *dst) {

}

int pugfs_move(PUGFS *fs, const char *src, const char *dst) {

}

pugfs_fd pugfs_fopen(PUGFS *fs, const char *path, pugfs_file_mode_t mode) {

}

int pugfs_fclose(PUGFS *fs, pugfs_fd fd) {

}

int pugfs_fstat(PUGFS *fs, pugfs_fd fd, struct pugfs_filestat *stat) {

}

int pugfs_fseek(PUGFS *fs, pugfs_fd fd, int offset, pugfs_seek_mode_t whence) {

}

size_t pugfs_fread(PUGFS *fs, void *dst, size_t size, size_t nmemb, pugfs_fd fd) {

}

size_t pugfs_fwrite(PUGFS *fs, const void *src, size_t size, size_t nmemb, pugfs_fd fd) {

}