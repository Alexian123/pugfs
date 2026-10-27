# pugfs

## Layout

### Sections

1. Boot sector containing the header
2. Reserved sectors
3. File Table
4. Raw file data, including the root directory

### Header
```c
struct pugfs_header
{
    uint8_t padding[3];                 // short jump + nop (on bootable disks)
    uint32_t magic;                     // 0xBEEFFEED
    uint16_t version;
    uint16_t sector_size;
    uint16_t header_size;
    uint16_t flags;
    uint32_t total_sectors;             // total sectors in the image
    uint32_t reserved_sectors;          // number of reserved sectors including the header
    uint32_t ftable_start_sector;       // start of the File Table (LBA)
    uint32_t ftable_sectors;            // size of the FTable
    uint32_t data_start_sector;         // start of the data region (LBA)
    uint32_t data_sectors;              // size of the data region
    uint32_t root_dir_starting_index;   // index into the FTable
    uint32_t checksum;
} __attribute__((packed));
```

### File Table

- 1 DW entry per sector
- Entry 0 is reserved so the index can start at 1

| Ftable Value | Meaning |
|---|---|
| `0x00000000` | Free |
| `0xFFFFFFFC` | End of chain |
| `0xFFFFFFFD` | Reserved |
| `0xFFFFFFFE` | Bad sector |
| `0xFFFFFFFF` | Invalid/unallocated reference |
| `0x00000001` – `0xFFFFFFFB` | Next sector index |

### Directories

- Contain directory entry items
- Entry names must be padded with null bytes

```c
struct pugfs_dir_entry
{
    uint8_t name[PUGFS_FILENAME_BYTES];  // 32 bytes including null terminator
    uint16_t attributes;                 // 2 bytes
    uint8_t type;                        // 1 byte
    uint32_t size;                       // 4 bytes
    uint32_t creation_ts;                // 4 bytes
    uint32_t last_access_ts;             // 4 bytes
    uint32_t starting_sector_index;      // 4 bytes
    uint8_t reserved[13];                // 13 bytes
} __attribute__((packed));               // total 64 bytes
```

| Entry Type | Meaning |
|---|---|
| `0xFF` | End of directory |
| `0xFE` | Bad entry |
| `0xFD` | Directory |
| `0xFC` | Regular file |