#include "disk_img.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct disk_img
{
    FILE *host_file;
    int sector_size;
    int total_size;
};

struct disk_img_stream
{
    DISK_IMG *img;
    int pos;
    char *buf;
};

DISK_IMG* disk_img_open(const char *host_path, int sector_size) {
    FILE *f = fopen(host_path, "r+");
    if (!f) {
        return NULL;
    }

    DISK_IMG *img = (DISK_IMG*)malloc(sizeof(struct disk_img));
    if (!img) {
        fclose(f);
        return NULL;
    }

    img->host_file = f;
    img->sector_size = sector_size;

    fseek(f, 0, SEEK_END);
    img->total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    return img;
}

void disk_img_close(DISK_IMG *img) {
    if (img) {
        if (img->host_file) {
            fclose(img->host_file);
        }
        free(img);
    }
}

int disk_img_get_sector_size(DISK_IMG *img) {
    if (!img) return -1;
    return img->sector_size;
}

int disk_img_get_total_size(DISK_IMG *img) {
    if (!img) return -1;
    return img->total_size;
}

int disk_img_read_sectors(DISK_IMG *img, uint32_t lba, int count, void *dst) {
    if (!img || !dst || count < 0) return -1;
    fseek(img->host_file, lba * img->sector_size, SEEK_SET);
    size_t res = fread(dst, img->sector_size, count, img->host_file);
    fseek(img->host_file, 0, SEEK_SET);
    return (int)res;
}

DISK_IMG_STREAM* disk_img_stream_create(DISK_IMG *img) {
    if (!img) return NULL;
    DISK_IMG_STREAM *stream = (DISK_IMG_STREAM*)malloc(sizeof(struct disk_img_stream));
    if (!stream) return NULL;

    stream->buf = (char*)malloc(img->sector_size);
    if (!stream->buf) {
        free(stream);
        return NULL;
    }

    stream->img = img;
    stream->pos = 0;
    return stream;
}

void disk_img_stream_destroy(DISK_IMG_STREAM *stream) {
    if (stream) {
        if (stream->buf) {
            free(stream->buf);
        }
        free(stream);
    }
}

int disk_img_stream_seekb(DISK_IMG_STREAM *stream, int byte_pos) {
    if (!stream || byte_pos < 0) return -1;
    stream->pos = byte_pos;
    return 0;
}

int disk_img_stream_readb(DISK_IMG_STREAM *stream, int count, void *dst) {
    if (!stream || !stream->img || count < 0 || !dst)
        return -1;

    if (count == 0) return 0;

    DISK_IMG *img = stream->img;

    if (img->sector_size <= 0) return -1;

    if (stream->pos < 0 || stream->pos > img->total_size) return -1;

    // Don't read beyond the end of the image
    int available = img->total_size - stream->pos;
    if (count > available) {
        count = available;
    }

    int bytes_read = 0;
    while (bytes_read < count) {
        // calculate the sector and offset within that sector
        int current_pos = stream->pos;
        uint32_t lba = (uint32_t)(current_pos / img->sector_size);
        int offset = current_pos % img->sector_size;

        // read the sector into temporary buffer
        int res = disk_img_read_sectors(img, lba, 1, stream->buf);
        if (res != 1) break;

        // determine how many bytes we can copy from this sector
        int available_in_sector = img->sector_size - offset;
        int remaining = count - bytes_read;

        int to_read = remaining < available_in_sector ? 
            remaining : available_in_sector;
        memcpy(dst, stream->buf + offset, to_read);

        dst += to_read;
        stream->pos += to_read;
        bytes_read += to_read;
    }

    return bytes_read;
}