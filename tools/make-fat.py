#!/usr/bin/env python3
"""
OmniOS FAT filesystem image builder (pure Python, no mtools/dosfstools).

Builds a small FAT16 "superfloppy" image containing an EFI System Partition:
    /EFI/BOOT/BOOTX64.EFI

This image is what the ISO's El Torito EFI boot catalog entry points at:
UEFI firmware mounts it as a FAT volume and executes \\EFI\\BOOT\\BOOTX64.EFI
directly (the Linux kernel carries its own EFI stub, so no bootloader).

The authoritative CI path installs mtools/xorriso and uses those; this file
exists so `scripts/build.sh iso` also works on a minimal host with no ISO
tooling beyond Python.

Usage: tools/make-fat.py KERNEL_FILE OUTPUT_IMG [SPARE_MB]
"""
import os
import struct
import sys

SECTOR = 512


def main():
    kernel = sys.argv[1]
    out = sys.argv[2]
    spare_mb = int(sys.argv[3]) if len(sys.argv) > 3 else 16

    ksize = os.path.getsize(kernel)
    # FAT16 layout; clusters of 4 KiB. Give room for the kernel + directories.
    sectors_per_cluster = 4
    cluster_bytes = SECTOR * sectors_per_cluster
    needed_bytes = ksize + spare_mb * 1024 * 1024
    total_clusters = (needed_bytes + cluster_bytes - 1) // cluster_bytes
    # FAT16 needs > 4085 clusters. Clamp below 16384 clusters so the image
    # stays under 65535 * 512 bytes (the 16-bit sector-count limit of the
    # El Torito boot entry that references it in the ISO).
    total_clusters = max(total_clusters, 8192)
    total_clusters = min(total_clusters, 16382)
    total_sectors = total_clusters * sectors_per_cluster

    reserved = 1
    fats = 2
    bytes_per_fat_entry = 2
    root_entries = 512
    root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR

    # FAT size: room for cluster_count + 2 entries, per FAT.
    fat_sectors = ((total_clusters + 2) * bytes_per_fat_entry + SECTOR - 1) // SECTOR

    data_start = reserved + fats * fat_sectors + root_sectors
    data_clusters = (total_sectors - data_start) // sectors_per_cluster
    # recompute totals so the data region matches cluster math
    total_sectors = data_start + data_clusters * sectors_per_cluster

    fat_size_bytes = fat_sectors * SECTOR
    fat = bytearray(fat_size_bytes * fats)
    # cluster 0 and 1 in each FAT: media descriptor + EOC
    _put_fat(fat, 0, fat_sectors, 0, 0xFFFFFFF8)
    _put_fat(fat, 1, fat_sectors, 0, 0xFFFFFFFF)

    # ---- BPB (FAT16) ------------------------------------------------------
    bpb = bytearray(SECTOR)
    # bytes 0..2: jump
    bpb[0:3] = b"\xEB\x3C\x90"
    bpb[3:11] = b"OMNIOS  "          # OEM
    s16 = struct.Struct("<H")
    s32 = struct.Struct("<I")
    def put16(off, v): bpb[off:off+2] = s16.pack(v)
    def put32(off, v): bpb[off:off+4] = s32.pack(v)
    put16(11, SECTOR)                # bytes per sector
    bpb[13] = sectors_per_cluster    # sectors per cluster
    put16(14, reserved)              # reserved sectors
    bpb[16] = fats                   # number of FATs
    put16(17, root_entries)          # root entries
    put16(19, 0)                     # total sectors 16 (0 -> use 32)
    bpb[21] = 0xF8                   # media
    put16(22, fat_sectors)           # sectors per FAT
    put16(24, 0)                     # sectors per track
    put16(26, 0)                     # heads
    put32(28, 0)                     # hidden sectors
    put32(32, total_sectors)         # total sectors 32
    # extended BPB (FAT16)
    bpb[36] = 0x80                   # drive number
    bpb[37] = 0                      # reserved
    bpb[38] = 0x29                   # EBPB signature
    bpb[39:43] = s32.pack(0x1337)    # volume serial
    bpb[43:54] = b"OMNIOSESP".ljust(11)  # volume label (11 bytes, no NUL)
    bpb[54:62] = b"FAT16   "         # FS type (8)
    bpb[510:512] = b"\x55\xAA"       # boot signature

    # ---- root directory (fixed region) -------------------------------------
    root = bytearray(root_sectors * SECTOR)

    # filename 8.3 + attrs + cluster helpers
    def dirent(name, ext, attr, cluster, size=0):
        e = bytearray(32)
        nm = name[:8].upper().ljust(8)
        ex = ext[:3].upper().ljust(3)
        e[0:8] = nm.encode("ascii", "ignore")
        e[8:11] = ex.encode("ascii", "ignore")
        e[11] = attr
        e[26:28] = s16.pack(cluster & 0xFFFF)
        e[28:32] = s32.pack(size)
        return e

    # allocate clusters sequentially
    next_free = [2]  # first data cluster index is 2

    def alloc_cluster():
        c = next_free[0]
        next_free[0] += 1
        return c

    def chain_put(prev, cur):
        _put_fat(fat, prev, fat_sectors, 0, cur)

    def end_chain(cluster):
        _put_fat(fat, cluster, fat_sectors, 0, 0xFFFFFFFF)

    def write_clusters(cluster, data):
        """Write `data` across clusters starting at `cluster`; chain them."""
        off = cluster_offset(cluster)
        end_cluster = cluster + (len(data) + cluster_bytes - 1) // cluster_bytes
        # chain
        for c in range(cluster, end_cluster - 1):
            chain_put(c, c + 1)
        if end_cluster - 1 >= 2:
            end_chain(end_cluster - 1)
        # write into the data region buffer
        ds = off - data_start * SECTOR
        _data[ds:ds + len(data)] = data
        return end_cluster - 1

    # allocate the full data region buffer
    _data = bytearray(data_clusters * cluster_bytes)

    def cluster_offset(c):
        return (data_start + (c - 2) * sectors_per_cluster) * SECTOR

    # /EFI directory cluster
    efi_cluster = alloc_cluster()
    # /EFI/BOOT directory cluster
    boot_cluster = alloc_cluster()
    # /EFI/BOOT/BOOTX64.EFI file clusters
    file_start = next_free[0]  # first cluster of kernel data

    # root dir entries: volume label + EFI
    root[0:32] = dirent("OMNIOSESP", "   ", 0x08, cluster=0, size=0)  # volume label
    root[32:64] = dirent("EFI", "   ", 0x10, cluster=efi_cluster)

    # /EFI directory: . .. BOOT
    efi = bytearray(cluster_bytes)
    efi[0:32] = dirent(".", "   ", 0x10, efi_cluster)
    efi[32:64] = dirent("..", "  ", 0x10, 0)
    efi[64:96] = dirent("BOOT", "   ", 0x10, cluster=boot_cluster)
    write_clusters(efi_cluster, efi)

    # /EFI/BOOT directory: . .. BOOTX64.EFI
    boot = bytearray(cluster_bytes)
    boot[0:32] = dirent(".", "   ", 0x10, boot_cluster)
    boot[32:64] = dirent("..", "  ", 0x10, efi_cluster)
    boot[64:96] = dirent("BOOTX64", "EFI", 0x20, cluster=file_start, size=ksize)
    write_clusters(boot_cluster, boot)

    # kernel file data
    with open(kernel, "rb") as kf:
        kdata = kf.read()
    write_clusters(file_start, kdata)

    # ---- assemble the image ------------------------------------------------
    img = bytearray(total_sectors * SECTOR)
    img[0:SECTOR] = bpb
    for i in range(fats):
        a = (reserved + i * fat_sectors) * SECTOR
        img[a:a + fat_size_bytes] = fat[i * fat_size_bytes:(i + 1) * fat_size_bytes]
    rstart = (reserved + fats * fat_sectors) * SECTOR
    img[rstart:rstart + len(root)] = root
    dstart = data_start * SECTOR
    img[dstart:dstart + len(_data)] = _data

    with open(out, "wb") as f:
        f.write(img)
    print("make-fat: wrote %s (%.1f MiB, %d clusters, FAT16)"
          % (out, len(img) / 1048576.0, data_clusters))


def _put_fat(fat, cluster, fat_sectors, fat_index, value):
    off = fat_index * fat_sectors * SECTOR + cluster * 2
    fat[off:off+2] = struct.pack("<H", value & 0xFFFF)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    main()
