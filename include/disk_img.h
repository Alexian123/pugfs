#ifndef __DISK_IMG_H
#define __DISK_IMG_H

#include <stdint.h>

typedef struct disk_img DISK_IMG;
typedef struct disk_img_stream DISK_IMG_STREAM;

DISK_IMG* disk_img_open(const char *path, int sector_size);
void disk_img_close(DISK_IMG *img);

int disk_img_get_sector_size(DISK_IMG *img);
int disk_img_get_total_size(DISK_IMG *img);

int disk_img_read_sectors(DISK_IMG *img, uint32_t lba, int count, void *dst);

DISK_IMG_STREAM* disk_img_stream_create(DISK_IMG *img);
void disk_img_stream_destroy(DISK_IMG_STREAM *stream);

int disk_img_stream_seekb(DISK_IMG_STREAM *stream, int byte_pos);
int disk_img_stream_readb(DISK_IMG_STREAM *stream, int count, void *dst);

#endif