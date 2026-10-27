#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pugfs.h"

#define IMAGE_PATH "./test.img"
#define SECTOR_SIZE 512

int main(void) {
    DISK_IMG *img = disk_img_open(IMAGE_PATH, SECTOR_SIZE);
    if (!img) {
        fprintf(stderr, "Failed to open image: %s\n", IMAGE_PATH);
        exit(EXIT_FAILURE);
    }
    
    DISK_IMG_STREAM *stream = disk_img_stream_create(img);
    if (!stream) {
        disk_img_close(img);
        fprintf(stderr, "Failed to create stream\n");
        exit(EXIT_FAILURE);
    }

    char buf[100000];
    memset(buf, 0xFF, sizeof(buf));

    disk_img_stream_readb(stream, 5, buf + 2);
    for (int i = 0; i < 10; ++i) {
        printf("%hhu ", buf[i]);
    }
    printf("\n");

    PUGFS *fs = pugfs_format(img, 2);
    pugfs_destroy(fs);

    int res = pugfs_detect(img);
    if (res == 0) {
        printf("Pugfs detected on disk image: %s\n", IMAGE_PATH);
    }

    disk_img_stream_destroy(stream);
    disk_img_close(img);
    return 0;
}