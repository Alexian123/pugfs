#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pugfs.h"
#include "shell.h"

#define MIN_SECTOR_SIZE 512
#define MAX_SECTOR_SIZE 4096

#define DEFAULT_RESERVED_SECTORS 100
#define DEFAULT_SECTORS_PER_CLUSTER 1

uint32_t sector_size = 0;
const char *image_path = NULL;

int main(int argc, char **argv) {
    
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <sector_size> <path_to_image>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    sscanf(argv[1], "%u", &sector_size);
    image_path = argv[2];

    if (sector_size < MIN_SECTOR_SIZE || sector_size > MAX_SECTOR_SIZE || (sector_size % 512) != 0) {
        fprintf(stderr, "Invalid sector size: %u\n", sector_size);
        exit(EXIT_FAILURE);
    }

    DISK_IMG *img = disk_img_open(image_path, sector_size);
    if (!img) {
        fprintf(stderr, "Failed to open image: %s\n", image_path);
        exit(EXIT_FAILURE);
    }

    PUGFS *fs = pugfs_mount(img);
    if (fs) {
        printf("Successfully mounted pugfs image: %s\n", image_path);
    } else {
        printf("Failed to mount image: %s\nPossibly missing or corrupt file system\n", image_path);
        printf("Do you want to format the image? WARNING: all data will be lost!\n");
        printf("(y/n): ");

        int choice = fgetc(stdin);
        int c; 
        while ((c = getchar()) != '\n' && c != EOF);
        choice = tolower(choice);
        switch (choice)
        {
        case 'y':
            printf("Formatting image with pugfs...\nReserved sectors: %d\nCluster size: %d\n", DEFAULT_RESERVED_SECTORS, DEFAULT_SECTORS_PER_CLUSTER);
            pugfs_format(img, DEFAULT_RESERVED_SECTORS, DEFAULT_SECTORS_PER_CLUSTER);
            break;

        case 'n':
            printf("Exiting...\n");
            exit(EXIT_SUCCESS);
            break;
        
        default:
            fprintf(stderr, "Invalid option: %c\nExiting...\n", choice);
            exit(EXIT_FAILURE);
            break;
        }

        fs = pugfs_mount(img);
        if (!fs) {
            fprintf(stderr, "Failed to format image: %s\nExiting...\n", image_path);
            exit(EXIT_FAILURE);
        }
        printf("Image successfully formatted and mounted!\n");
    }

    shell_start(fs, img);

    return 0;
}