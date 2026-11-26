#ifndef __PUGFS_H
#define __PUGFS_H

#include "disk_img.h"
#include "path.h"

#include <stdlib.h>

#define PUGFS_FILENAME_BYTES 32     // must be padded with '\0'

typedef enum
{
    PUGFS_FILE_MODE_READ,
    PUGFS_FILE_MODE_WRITE,
    PUGFS_FILE_MODE_READWRITE,
    PUGFS_FILE_MODE_APPEND
} pugfs_file_mode_t;

typedef enum
{
    PUGFS_SEEK_SET,
    PUGFS_SEEK_CUR,
    PUGFS_SEEK_END
} pugfs_seek_mode_t;

typedef enum
{
    PUGFS_FILE_FILE,
    PUGFS_FILE_DIR
} pugfs_file_type_t;

typedef int pugfs_fd;

typedef struct pugfs PUGFS;

struct pugfs_filestat
{
    char name[PUGFS_FILENAME_BYTES + 1];
    pugfs_file_type_t type;
    uint64_t size;
    uint32_t creation_ts;
    uint32_t last_access_ts;
    uint8_t attributes;
};

int pugfs_format(DISK_IMG *img, uint32_t reserved_sectors, uint16_t sectors_per_cluster);
PUGFS* pugfs_mount(DISK_IMG *img);
void pugfs_destroy(PUGFS *fs);

int pugfs_touch(PUGFS *fs, PATH *path);
int pugfs_delete(PUGFS *fs, PATH *path);
int pugfs_trunc(PUGFS *fs, PATH *path, uint64_t new_size);
int pugfs_mkdir(PUGFS *fs, PATH *path);
int pugfs_rmdir(PUGFS *fs, PATH *path);
int pugfs_copy(PUGFS *fs, PATH *src, PATH *dst, int overwrite);
int pugfs_move(PUGFS *fs, PATH *src, PATH *dst, int overwrite);

int pugfs_lsdir_begin(PUGFS *fs, PATH *path);
int pugfs_lsdir_next(PUGFS *fs, struct pugfs_filestat *stat_out); // return 1 if ok, 0 if EOD, <0 if error

pugfs_fd pugfs_fopen(PUGFS *fs, PATH *path, pugfs_file_mode_t mode);
int pugfs_fclose(PUGFS *fs, pugfs_fd fd);
int pugfs_fstat(PUGFS *fs, PATH *path, struct pugfs_filestat *stat_out);
int pugfs_fseek(PUGFS *fs, pugfs_fd fd, int offset, pugfs_seek_mode_t whence);
size_t pugfs_fread(PUGFS *fs, void *dst, size_t size, size_t nmemb, pugfs_fd fd);
size_t pugfs_fwrite(PUGFS *fs, const void *src, size_t size, size_t nmemb, pugfs_fd fd);

#endif