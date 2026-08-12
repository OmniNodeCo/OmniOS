#!/bin/bash
set -e

echo "=== Building Custom ISO ==="

mkdir -p out

sudo mksquashfs iso/custom/squashfs-root iso/custom/casper/filesystem.squashfs -comp xz -b 1M -noappend

sudo xorriso -as mkisofs \
  -r -V "Ubuntu-Custom" \
  -J -l \
  -b isolinux/isolinux.bin \
  -c isolinux/boot.cat \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  -o out/ubuntu-custom-$(date +%Y%m%d).iso \
  iso/custom

echo "✅ Custom ISO created in out/"