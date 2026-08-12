#!/bin/bash
set -e

UBUNTU_VERSION="24.04.1"
UBUNTU_ISO="ubuntu-${UBUNTU_VERSION}-desktop-amd64.iso"
DOWNLOAD_URL="https://releases.ubuntu.com/${UBUNTU_VERSION}/${UBUNTU_ISO}"

echo "=== Downloading Ubuntu ISO ==="
mkdir -p iso

if [ -f "iso/$UBUNTU_ISO" ]; then
  echo "✅ Using existing ISO: iso/$UBUNTU_ISO"
else
  wget -c "$DOWNLOAD_URL" -O "iso/$UBUNTU_ISO"
  echo "✅ Download complete"
fi