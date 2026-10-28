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

    PUGFS *fs = pugfs_format(img, 2, 1);
    pugfs_destroy(fs);

    fs = pugfs_load(img);
    if (fs) {
        printf("Pugfs detected on disk image: %s\n", IMAGE_PATH);
    } else {
        printf("No fs\n");
    }

    disk_img_close(img);
    return 0;
}