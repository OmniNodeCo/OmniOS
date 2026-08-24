#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
command -v docker >/dev/null 2>&1 || {
    echo 'ERROR: Docker is required for the container build.' >&2
    exit 1
}

mkdir -p "$ROOT/build/downloads" "$ROOT/out"
exec docker run --rm --privileged \
    --volume "$ROOT:/workspace" \
    --workdir /workspace \
    --env HOST_UID="$(id -u)" \
    --env HOST_GID="$(id -g)" \
    --env SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}" \
    debian:13-slim \
    bash -Eeuo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install --yes --no-install-recommends \
            ca-certificates debootstrap dosfstools fdisk git gnupg grub-efi-amd64-bin \
            grub-pc-bin isolinux librsvg2-bin live-build mtools rsync shim-signed squashfs-tools \
            syslinux-common xorriso zstd
        scripts/build.sh build
    '
