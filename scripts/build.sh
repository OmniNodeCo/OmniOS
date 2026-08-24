#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
BUILD_ROOT="${BUILD_ROOT:-$ROOT/build}"
WORK_DIR="$BUILD_ROOT/work"
CACHE_DIR="$BUILD_ROOT/cache"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
VERSION="${OMNIOS_VERSION:-1.0}"
ARCH="${OMNIOS_ARCH:-amd64}"
OUTPUT_BASENAME="OmniOS-${VERSION}-${ARCH}"
ACTION="${1:-build}"

run_root() {
    if (( EUID == 0 )); then
        "$@"
    else
        sudo "$@"
    fi
}

require_build_tools() {
    local missing=()
    local command_name
    for command_name in lb debootstrap rsvg-convert xorriso mksquashfs rsync sha256sum; do
        command -v "$command_name" >/dev/null 2>&1 || missing+=("$command_name")
    done
    if (( ${#missing[@]} )); then
        printf 'ERROR: missing build commands: %s\n' "${missing[*]}" >&2
        printf 'Run scripts/bootstrap-host.sh or scripts/build-container.sh.\n' >&2
        exit 1
    fi
}

prepare_workspace() {
    mkdir -p "$BUILD_ROOT" "$CACHE_DIR" "$OUT_DIR"
    if [[ -e "$WORK_DIR" ]]; then
        run_root rm -rf -- "$WORK_DIR"
    fi
    mkdir -p "$WORK_DIR"
    cp -a "$ROOT/auto" "$ROOT/config" "$WORK_DIR/"
    ln -s ../cache "$WORK_DIR/cache"
}

prepare_bootloader_branding() {
    local template_root="${LIVE_BUILD_SHARE:-/usr/share/live/build}/bootloaders"
    mkdir -p "$WORK_DIR/config/bootloaders"
    for loader in isolinux grub-pc; do
        [[ -d "$template_root/$loader" ]] || {
            echo "ERROR: live-build bootloader template missing: $loader" >&2
            exit 1
        }
        cp -a "$template_root/$loader" "$WORK_DIR/config/bootloaders/"
    done

    cp "$ROOT/branding/boot-splash.svg" \
        "$WORK_DIR/config/bootloaders/isolinux/splash.svg"
    rsvg-convert --width 640 --height 480 \
        "$ROOT/branding/boot-splash.svg" \
        --output "$WORK_DIR/config/bootloaders/grub-pc/splash.png"

    find "$WORK_DIR/config/bootloaders" -type f \
        \( -name '*.cfg' -o -name '*.txt' -o -name '*.theme' \) \
        -exec sed -i \
            -e 's/Debian GNU\/Linux/OmniOS/g' \
            -e 's/Debian Live/OmniOS/g' {} +
}

configure() {
    require_build_tools
    prepare_workspace
    prepare_bootloader_branding
    (
        cd "$WORK_DIR"
        run_root env \
            SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || date +%s)}" \
            ./auto/config
    )
}

build_image() {
    configure
    run_root rm -f -- "$OUT_DIR/$OUTPUT_BASENAME".*
    local log_file="$BUILD_ROOT/live-build.log"
    set +e
    (
        cd "$WORK_DIR"
        run_root env \
            SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || date +%s)}" \
            ./auto/build
    ) 2>&1 | tee "$log_file"
    local build_status=${PIPESTATUS[0]}
    set -e
    (( build_status == 0 )) || exit "$build_status"

    local source_iso
    source_iso="$(find "$WORK_DIR" -maxdepth 1 -type f \( -name '*.hybrid.iso' -o -name '*.iso' \) -print | sort | tail -n 1)"
    [[ -n "$source_iso" && -f "$source_iso" ]] || {
        echo 'ERROR: live-build completed without producing an ISO.' >&2
        exit 1
    }

    run_root cp "$source_iso" "$OUT_DIR/$OUTPUT_BASENAME.iso"
    local source_packages
    source_packages="$(find "$WORK_DIR" -maxdepth 1 -type f -name '*.packages' -print | sort | tail -n 1 || true)"
    if [[ -n "$source_packages" && -f "$source_packages" ]]; then
        run_root cp "$source_packages" "$OUT_DIR/$OUTPUT_BASENAME.packages"
    fi

    run_root chown "${HOST_UID:-$(id -u)}:${HOST_GID:-$(id -g)}" "$OUT_DIR"/*
    (
        cd "$OUT_DIR"
        sha256sum "$OUTPUT_BASENAME.iso" > "$OUTPUT_BASENAME.iso.sha256"
    )
    xorriso -indev "$OUT_DIR/$OUTPUT_BASENAME.iso" -pvd_info > "$OUT_DIR/$OUTPUT_BASENAME.iso-info.txt" 2>&1
    printf 'Built %s\n' "$OUT_DIR/$OUTPUT_BASENAME.iso"
}

clean() {
    if [[ -e "$WORK_DIR" ]]; then
        run_root rm -rf -- "$WORK_DIR"
    fi
    rm -rf -- "$OUT_DIR"
    if [[ "${1:-}" == --purge-cache ]]; then
        run_root rm -rf -- "$CACHE_DIR"
    fi
}

case "$ACTION" in
    build) build_image ;;
    config|parse) configure ;;
    validate|checkout) "$ROOT/scripts/check-project.sh" ;;
    clean) clean "${2:-}" ;;
    *)
        echo 'Usage: scripts/build.sh [build|config|validate|clean [--purge-cache]]' >&2
        exit 2
        ;;
esac
