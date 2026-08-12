#!/bin/bash
set -e

echo "=== Customizing Ubuntu ==="

ISO_FILE=$(ls iso/ubuntu-*.iso 2>/dev/null | head -1)

if [ -z "$ISO_FILE" ]; then
  echo "❌ No Ubuntu ISO found in iso/ folder"
  exit 1
fi

echo "Using ISO: $ISO_FILE"

# Clean previous work
sudo rm -rf iso/custom /mnt/iso 2>/dev/null || true
mkdir -p iso/custom

# Mount ISO
sudo mkdir -p /mnt/iso
sudo mount -o loop,ro "$ISO_FILE" /mnt/iso

# Check for squashfs (new Ubuntu 24.04 layout)
SQUASHFS_PATH="/mnt/iso/casper/filesystem.squashfs"

if [ ! -f "$SQUASHFS_PATH" ]; then
  echo "❌ Could not find filesystem.squashfs inside the ISO"
  echo "ISO structure:"
  ls /mnt/iso/casper/ || true
  sudo umount /mnt/iso
  exit 1
fi

echo "Extracting squashfs..."
sudo unsquashfs -d iso/custom/squashfs-root "$SQUASHFS_PATH"

# Copy the rest of the ISO contents
echo "Copying ISO contents..."
sudo rsync -a --exclude='casper/filesystem.squashfs' /mnt/iso/ iso/custom/

# === CUSTOMIZATIONS ===
echo "Installing packages inside chroot..."
sudo chroot iso/custom/squashfs-root /bin/bash -c "
    export DEBIAN_FRONTEND=noninteractive
    apt update -y
    apt install -y vim htop neofetch curl wget git
"

# Set hostname
echo "ubuntu-custom" | sudo tee iso/custom/squashfs-root/etc/hostname > /dev/null

sudo umount /mnt/iso
echo "✅ Customization completed successfully"