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

        # deb.debian.org is a CDN and occasionally drops a connection mid
        # transfer. Without retries a single broken pipe fails a build that
        # downloads several thousand packages.
        cat > /etc/apt/apt.conf.d/80-omnios-retries <<APT
Acquire::Retries "10";
Acquire::http::Timeout "120";
Acquire::https::Timeout "120";
Acquire::http::Pipeline-Depth "0";
Acquire::Queue-Host "2";
APT

        if [[ -f /etc/apt/sources.list.d/debian.sources ]]; then
            sed -i "s/^Components: main$/Components: main contrib non-free-firmware/" \
                /etc/apt/sources.list.d/debian.sources
        fi
        apt-get update
        apt-get install --yes --no-install-recommends \
            ca-certificates debootstrap dosfstools fdisk git gnupg grub-efi-amd64-bin \
            grub-pc-bin isolinux librsvg2-bin live-build mtools rsync shim-signed squashfs-tools \
            syslinux-common xorriso zstd
        scripts/check-packages.sh
        scripts/build.sh build
    '
