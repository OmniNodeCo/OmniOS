#!/bin/bash
set -e

echo "=== Customizing Ubuntu ==="

ISO_FILE=$(ls iso/ubuntu-*.iso | head -1)

sudo mkdir -p /mnt/iso
sudo mount -o loop "$ISO_FILE" /mnt/iso

sudo rm -rf iso/custom
sudo mkdir -p iso/custom
sudo rsync -a --exclude=casper/filesystem.squashfs /mnt/iso/ iso/custom/

sudo unsquashfs /mnt/iso/casper/filesystem.squashfs
sudo mv squashfs-root iso/custom/squashfs-root

sudo chroot iso/custom/squashfs-root bash -c "
  apt update -y
  apt install -y vim htop neofetch
"

echo "ubuntu-custom" | sudo tee iso/custom/squashfs-root/etc/hostname

sudo umount /mnt/iso
echo "✅ Customization done"