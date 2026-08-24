#!/usr/bin/env bash
set -Eeuo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "Automatic host setup currently supports Debian/Ubuntu systems." >&2
    exit 1
fi

sudo apt-get update
sudo apt-get install --yes \
    ca-certificates curl debootstrap dosfstools fdisk git gnupg grub-efi-amd64-bin \
    grub-pc-bin isolinux librsvg2-bin live-build mtools ovmf qemu-system-x86 rsync \
    shellcheck shim-signed squashfs-tools syslinux-common xorriso zstd

cat <<'TXT'
Host setup complete.
Build directly with: scripts/build.sh build
For the recommended reproducible Debian 13 container build, install Docker and run:
  scripts/build-container.sh
TXT
