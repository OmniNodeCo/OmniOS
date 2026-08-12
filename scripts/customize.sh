#!/bin/bash
set -e

echo "=== Customizing Ubuntu ==="

ISO_FILE=$(ls iso/ubuntu-*.iso 2>/dev/null | head -1)

if [ -z "$ISO_FILE" ]; then
  echo "❌ No Ubuntu ISO found in iso/ folder"
  exit 1
fi

echo "Using ISO: $ISO_FILE"

sudo rm -rf iso/custom /mnt/iso 2>/dev/null || true
mkdir -p iso/custom

sudo mkdir -p /mnt/iso
sudo mount -o loop,ro "$ISO_FILE" /mnt/iso

# Detect correct squashfs (Ubuntu 24.04 uses different names)
SQUASHFS_PATH=""

if [ -f "/mnt/iso/casper/filesystem.squashfs" ]; then
  SQUASHFS_PATH="/mnt/iso/casper/filesystem.squashfs"
elif [ -f "/mnt/iso/casper/minimal.standard.squashfs" ]; then
  SQUASHFS_PATH="/mnt/iso/casper/minimal.standard.squashfs"
else
  echo "❌ Could not find any valid squashfs in the ISO"
  echo "Available files in casper/:"
  ls /mnt/iso/casper/ || true
  sudo umount /mnt/iso
  exit 1
fi

echo "Using squashfs: $SQUASHFS_PATH"

echo "Extracting squashfs..."
sudo unsquashfs -d iso/custom/squashfs-root "$SQUASHFS_PATH"

echo "Copying ISO contents..."
sudo rsync -a --exclude='casper/*.squashfs' /mnt/iso/ iso/custom/

# === CUSTOMIZATIONS ===
echo "Installing packages..."
sudo chroot iso/custom/squashfs-root /bin/bash -c "
    export DEBIAN_FRONTEND=noninteractive
    apt update -y
    apt install -y vim htop neofetch curl wget git
"

echo "ubuntu-custom" | sudo tee iso/custom/squashfs-root/etc/hostname > /dev/null

sudo umount /mnt/iso
echo "✅ Customization completed successfully"