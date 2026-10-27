# pugfs

## Layout

1. 512B boot sector containing a header (like FAT)
2. Reserved sectors (n)
3. File table (like FAT), but 1 DW entry per sector, entry 0 is reserved
4. Raw file data, including root directory

| Ftable Value | Meaning |
|---|---|
| `0x00000000` | Free |
| `0xFFFFFFFC` | End of chain |
| `0xFFFFFFFD` | Reserved |
| `0xFFFFFFFE` | Bad sector |
| `0xFFFFFFFF` | Invalid/unallocated reference |
| `0x00000001` – `0xFFFFFFFB` | Next sector index |