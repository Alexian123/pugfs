# pugfs

## Layout

1. 512B boot sector containing a header (like FAT16)
2. Reserved sectors (n)
3. File table (like FAT)
4. Root directory
5. Raw file data