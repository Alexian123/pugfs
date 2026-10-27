#include <pugfs.h>
#include <stdio.h>

#define PUGFS_FILENAME_BYTES         32     // must be padded with '\0'
#define PUGFS_FTABLE_ENTRY_BYTES     4

#define PUGFS_FTABLE_ENTRY_FREE             0x00000000
#define PUGFS_FTABLE_ENTRY_EOC              0xFFFFFFFC  // end of chain
#define PUGFS_FTABLE_ENTRY_RESERVED         0xFFFFFFFD
#define PUGFS_FTABLE_ENTRY_BAD              0xFFFFFFFE
#define PUGfS_FTABLE_ENTRY_INVALID          0xFFFFFFFF
#define PUGFS_FTABLE_ENTRY_SECTOR_ID_MIN    0x00000001
#define PUGFS_FTABLE_ENTRY_SECTOR_ID_MAX    0xFFFFFFFB

#define PUGFS_DIR_ENTRY_TYPE_EOD        0xFF    // marks the end of directory, not a real file
#define PUGFS_DIR_ENTRY_TYPE_BAD        0xFE    // bad or corrupted entry
#define PUGFS_DIR_ENTRY_TYPE_FILE       0xFD    // file
#define PUGFS_DIR_ENTRY_TYPE_DIR        0xFC    // directory

struct pugfs_header
{
    uint32_t magic;                     // easy id -> should always be 0xBEEFFEED
    uint16_t version;
    uint16_t sector_size;
    uint16_t header_size;
    uint16_t flags;
    uint32_t total_sectors;             // total sectors in the image
    uint32_t reserved_sectors;          // number of sectors reserved after the header, including the header
    uint32_t ftable_start_sector;       // start of the File Table (LBA)
    uint32_t ftable_sectors;            // size of the FTable
    uint32_t data_start_sector;         // start of the data region (LBA)
    uint32_t data_sectors;              // size of the data region
    uint32_t root_dir_starting_index;   // index into the FTable
    uint32_t checksum;
} __attribute__((packed));;

struct pugfs_dir_entry
{
    uint8_t name[PUGFS_FILENAME_BYTES];  // 32 bytes including null terminator
    uint16_t attributes;                 // 2 bytes
    uint8_t type;                        // 1 byte
    uint32_t size;                       // 4 bytes
    uint32_t creation_ts;                // 4 bytes
    uint32_t last_access_ts;             // 4 bytes
    uint32_t starting_sector_index;      // 4 bytes
    uint8_t reserved[13];                // 13 bytes
} __attribute__((packed));

int pugfs_detect(DISK_IMG *img) {
    // check magic number

    // verify checksum
}

PUGFS* pugfs_format(DISK_IMG *img) {

}

PUGFS* pugfs_load(DISK_IMG *img) {

}

void pugfs_destroy(PUGFS *fs) {

}

int pugfs_touch(PUGFS *fs, const char *path) {

}

int pugfs_delete(PUGFS *fs, const char *path) {

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