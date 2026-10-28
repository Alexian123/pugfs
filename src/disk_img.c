#include "disk_img.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct disk_img
{
    FILE *host_file;
    uint32_t sector_size;
    uint32_t total_sectors;
};

struct disk_img_stream
{
    DISK_IMG *img;
    uint32_t sector;
    uint32_t offset;
    char *buf;
};

DISK_IMG* disk_img_open(const char *host_path, uint32_t sector_size) {
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
    long bytes = ftell(f);
    img->total_sectors = bytes / sector_size;
    if (bytes % sector_size != 0) {
        ++(img->total_sectors);
    }
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

int disk_img_get_total_sectors(DISK_IMG *img) {
    if (!img) return -1;
    return img->total_sectors;
}

int disk_img_read_sectors(DISK_IMG *img, uint32_t lba, int count, void *dst) {
    if (!img || !dst || count < 0) return -1;
    if (lba + count > img->total_sectors) return -1;
    fseek(img->host_file, lba * img->sector_size, SEEK_SET);
    size_t res = fread(dst, img->sector_size, count, img->host_file);
    fseek(img->host_file, 0, SEEK_SET);
    return (int)res;
}

int disk_img_write_sectors(DISK_IMG *img, uint32_t lba, int count, const void *src) {
    if (!img || !src || count < 0) return -1;
    if (lba + count > img->total_sectors) return -1;
    fseek(img->host_file, lba * img->sector_size, SEEK_SET);
    size_t res = fwrite(src, img->sector_size, count, img->host_file);
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
    stream->sector = 0;
    stream->offset = 0;
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

int disk_img_stream_seek(DISK_IMG_STREAM *stream, uint32_t sector, uint32_t offset) {
    if (!stream || !stream->img) return -1;
    DISK_IMG *img = stream->img;
    if (sector >= img->total_sectors || offset >= img->sector_size) return -1;
    stream->sector = sector;
    stream->offset = offset;
    return 0;
}

static void disk_img_stream_advance(DISK_IMG_STREAM *stream, uint32_t count) {
    stream->offset += count;
    stream->sector += stream->offset / stream->img->sector_size;
    stream->offset %= stream->img->sector_size;
}

int disk_img_stream_readb(DISK_IMG_STREAM *stream, int count, void *dst) {
    if (!stream || !stream->img || count < 0 || !dst)
        return -1;

    if (count == 0)
        return 0;

    DISK_IMG *img = stream->img;
    char *cdst = (char *)dst;
    int bytes_read = 0;

    while (bytes_read < count) {
        if (stream->sector >= img->total_sectors)
            break;

        int available = (int)img->sector_size - (int)stream->offset;
        int remaining = count - bytes_read;
        int to_read = remaining < available ? remaining : available;

        if (disk_img_read_sectors(img, stream->sector, 1, stream->buf) != 1)
            break;

        memcpy(cdst, stream->buf + stream->offset, (size_t)to_read);

        cdst += to_read;
        bytes_read += to_read;

        disk_img_stream_advance(stream, (uint32_t)to_read);
    }

    return bytes_read;
}

int disk_img_stream_writeb(DISK_IMG_STREAM *stream, int count, const void *src) {
    if (!stream || !stream->img || count < 0 || !src)
        return -1;

    if (count == 0)
        return 0;

    DISK_IMG *img = stream->img;
    const char *csrc = (const char *)src;
    int bytes_written = 0;

    while (bytes_written < count) {
        if (stream->sector >= img->total_sectors)
            break;

        int available = (int)img->sector_size - (int)stream->offset;
        int remaining = count - bytes_written;
        int to_write = remaining < available ? remaining : available;

        // Preserve the rest of the sector for partial-sector writes.
        if (stream->offset != 0 || to_write != (int)img->sector_size) {
            if (disk_img_read_sectors(img, stream->sector, 1, stream->buf) != 1)
                break;
        }

        memcpy(stream->buf + stream->offset, csrc, (size_t)to_write);

        if (disk_img_write_sectors(img, stream->sector, 1, stream->buf) != 1)
            break;

        csrc += to_write;
        bytes_written += to_write;

        disk_img_stream_advance(stream, (uint32_t)to_write);
    }

    return bytes_written;
}

int disk_img_stream_read(DISK_IMG_STREAM *stream, int sectors, void *dst) {
    if (!stream || !stream->img || !dst || sectors < 0)
        return -1;

    uint32_t sector_size = stream->img->sector_size;
    uint32_t total_sectors = stream->img->total_sectors;

    if (sector_size == 0) return -1;

    if ((uint64_t)stream->sector + sectors > total_sectors)
        return -1;

    char *cdst = (char *)dst;
    for (int i = 0; i < sectors; ++i) {
        int res = disk_img_stream_readb(stream, (int)sector_size, cdst);
        if (res != (int)sector_size)
            return i;
        cdst += sector_size;
    }

    return sectors;
}

int disk_img_stream_write(DISK_IMG_STREAM *stream, int sectors, const void *src) {
    if (!stream || !stream->img || !src || sectors < 0)
        return -1;

    uint32_t sector_size = stream->img->sector_size;
    uint32_t total_sectors = stream->img->total_sectors;

    if (sector_size == 0) return -1;

    if ((uint64_t)stream->sector + sectors > total_sectors)
        return -1;

    const char *csrc = (const char *)src;
    for (int i = 0; i < sectors; ++i) {
        int res = disk_img_stream_writeb(stream, (int)sector_size, csrc);
        if (res != (int)sector_size)
            return i;
        csrc += sector_size;
    }

    return sectors;
}