#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pugfs.h"

#define IMAGE_PATH "./test.img"
#define SECTOR_SIZE 512

#define MAX_ENTRIES 100

char list[MAX_ENTRIES * PUGFS_FILENAME_BYTES];

int main(void) {
    memset(list, 0, sizeof(list));

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
        int res = 0;

        res = pugfs_delete(fs, "0:/hello.txt");
        printf("delete result: %d\n", res);

        res = pugfs_touch(fs, "0:/hello.txt");
        printf("touch result: %d\n", res);

        res = pugfs_delete(fs, "0:/hello.txt");
        printf("delete result: %d\n", res);

        res = pugfs_touch(fs, "0:/other.sys");
        printf("touch result: %d\n", res);
        res = pugfs_touch(fs, "0:/program.exe");
        printf("touch result: %d\n", res);

        res = pugfs_touch(fs, "0:/hello.txt");
        printf("touch result: %d\n", res);
        res = pugfs_touch(fs, "0:/other.sys");
        printf("touch result: %d\n", res);
        res = pugfs_touch(fs, "0:/program.exe");
        printf("touch result: %d\n", res);

        res = pugfs_touch(fs, "0:/newdir/hello.txt");
        printf("touch result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir/second");
        printf("mkdir result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir/second/hello.txt");
        printf("mkdir result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir");
        printf("mkdir result: %d\n", res);
        res = pugfs_mkdir(fs, "0:/otherdir");
        printf("mkdir result: %d\n", res);
        res = pugfs_mkdir(fs, "0:/programdir");
        printf("mkdir result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir/second");
        printf("mkdir result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir/second");
        printf("mkdir result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir/second/hello.txt");
        printf("mkdir result: %d\n", res);

        res = pugfs_touch(fs, "0:/newdir/second/hello.txt/myfile.c");
        printf("touch result: %d\n", res);

        res = pugfs_lsdir(fs, "0:/", list, MAX_ENTRIES);
        printf("Found %d entries in root. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_lsdir(fs, "0:/newdir", list, MAX_ENTRIES);
        printf("Found %d entries in root/newdir. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_lsdir(fs, "0:/otherdir", list, MAX_ENTRIES);
        printf("Found %d entries in root/otherdir. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_lsdir(fs, "0:/programdir", list, MAX_ENTRIES);
        printf("Found %d entries in root/programdir. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_lsdir(fs, "0:/newdir/second", list, MAX_ENTRIES);
        printf("Found %d entries in root/second. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_lsdir(fs, "0:/newdir/second/hello.txt", list, MAX_ENTRIES);
        printf("Found %d entries in root/second/hello.txt. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_delete(fs, "0:/hello.txt");
        printf("delete result: %d\n", res);

        res = pugfs_lsdir(fs, "0:/", list, MAX_ENTRIES);
        printf("Found %d entries in root. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }

        res = pugfs_rmdir(fs, "0:/newdir");
        printf("rmdir result: %d\n", res);

        res = pugfs_mkdir(fs, "0:/newdir");
        printf("mkdir result: %d\n", res);

        res = pugfs_lsdir(fs, "0:/newdir", list, MAX_ENTRIES);
        printf("Found %d entries in root/newdir. Listing:\n", res);
        for (int i = 0; i < res * PUGFS_FILENAME_BYTES; i += PUGFS_FILENAME_BYTES) {
            printf("\t%s\n", list + i);
        }
        
    } else {
        printf("No fs\n");
    }

    disk_img_close(img);
    return 0;
}