#ifndef __PUGFS_H
#define __PUGFS_H

#include "disk_img.h"

#include <stdlib.h>

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

typedef unsigned int pugfs_fd;

typedef struct pugfs PUGFS;

struct pugfs_filestat
{
    pugfs_file_type_t type;
    uint32_t size;
    uint32_t creation_ts;
    uint32_t last_access_ts;
    uint8_t attributes;
};

int pugfs_detect(DISK_IMG *img);
PUGFS* pugfs_format(DISK_IMG *img);
PUGFS* pugfs_load(DISK_IMG *img);
void pugfs_destroy(PUGFS *fs);

int pugfs_touch(PUGFS *fs, const char *path);
int pugfs_delete(PUGFS *fs, const char *path);
int pugfs_mkdir(PUGFS *fs, const char *path);
int pugfs_rmdir(PUGFS *fs, const char *path);
int pugfs_lsdir(PUGFS *fs, const char *path, char **list_out, size_t start_index, size_t max_entries);
int pugfs_copy(PUGFS *fs, const char *src, const char *dst);
int pugfs_move(PUGFS *fs, const char *src, const char *dst);

pugfs_fd pugfs_fopen(PUGFS *fs, const char *path, pugfs_file_mode_t mode);
int pugfs_fclose(PUGFS *fs, pugfs_fd fd);
int pugfs_fstat(PUGFS *fs, pugfs_fd fd, struct pugfs_filestat *stat);
int pugfs_fseek(PUGFS *fs, pugfs_fd fd, int offset, pugfs_seek_mode_t whence);
size_t pugfs_fread(PUGFS *fs, void *dst, size_t size, size_t nmemb, pugfs_fd fd);
size_t pugfs_fwrite(PUGFS *fs, const void *src, size_t size, size_t nmemb, pugfs_fd fd);

#endif