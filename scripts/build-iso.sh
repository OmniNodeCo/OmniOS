#!/usr/bin/env bash
# Build a bootable OmniOS ISO by remastering an Ubuntu 24.04 Desktop ISO.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
SOURCE_ISO="${SOURCE_ISO:-}"
OUTPUT_ISO="${OUTPUT_ISO:-$REPO_ROOT/out/OmniOS-1.0-amd64.iso}"
WORK_DIR="${WORK_DIR:-$REPO_ROOT/build/iso-remaster}"
KEEP_WORK=false
MOUNTS=()

usage() {
  cat <<'EOF'
Usage: sudo scripts/build-iso.sh [options]

Build a bootable OmniOS desktop/install ISO from Ubuntu 24.04 Desktop.
Download the verified source first with scripts/download-ubuntu.sh.

Options:
  --source ISO       Source Ubuntu Desktop ISO (auto-detected under ./iso)
  --output ISO       Output path (default: out/OmniOS-1.0-amd64.iso)
  --work-dir DIR     Temporary build directory (default: build/iso-remaster)
  --keep-work        Keep extracted layers after a successful build
  -h, --help         Show this help

Environment:
  SQUASHFS_PROCESSORS  mksquashfs worker count (default: up to 4)
EOF
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

log() {
  printf '\n==> %s\n' "$*"
}

unmount_path() {
  local path="$1"
  if mountpoint --quiet "$path"; then
    umount --recursive "$path" 2>/dev/null || umount --lazy "$path"
  fi
}

cleanup() {
  local status=$?
  set +e
  for ((index=${#MOUNTS[@]} - 1; index >= 0; index--)); do
    unmount_path "${MOUNTS[index]}"
  done
  if [[ $status -ne 0 ]]; then
    printf '\nBuild failed. Working files remain at %s\n' "$WORK_DIR" >&2
  elif [[ "$KEEP_WORK" == false ]]; then
    rm -rf -- "$WORK_DIR"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mount_track() {
  mount "$@"
  MOUNTS+=("${*: -1}")
}

while (($# > 0)); do
  case "$1" in
    --source)
      (($# >= 2)) || fail '--source requires an ISO path'
      SOURCE_ISO="$2"
      shift 2
      ;;
    --output)
      (($# >= 2)) || fail '--output requires an ISO path'
      OUTPUT_ISO="$2"
      shift 2
      ;;
    --work-dir)
      (($# >= 2)) || fail '--work-dir requires a directory'
      WORK_DIR="$2"
      shift 2
      ;;
    --keep-work) KEEP_WORK=true; shift ;;
    -h|--help) usage; trap - EXIT HUP INT TERM; exit 0 ;;
    *) fail "unknown option: $1" ;;
  esac
done

((EUID == 0)) || fail 'the ISO remaster requires root; run it with sudo'
[[ "$(uname -m)" == x86_64 ]] || fail 'the AMD64 image must be built on an x86_64 Linux host'

required_commands=(awk chroot cp du find md5sum mount mountpoint mksquashfs realpath sed sha256sum sort stat unsquashfs xorriso)
for command_name in "${required_commands[@]}"; do
  command -v "$command_name" >/dev/null 2>&1 || fail "missing required command: $command_name"
done

if [[ -z "$SOURCE_ISO" ]]; then
  SOURCE_ISO="$(
    find "$REPO_ROOT/iso" -maxdepth 1 -type f -name 'ubuntu-*-desktop-amd64.iso' -print 2>/dev/null \
      | sort -V \
      | tail -n 1 \
      || true
  )"
fi
[[ -n "$SOURCE_ISO" && -f "$SOURCE_ISO" ]] || fail 'no source ISO found; run scripts/download-ubuntu.sh first'
SOURCE_ISO="$(realpath "$SOURCE_ISO")"
OUTPUT_ISO="$(realpath -m "$OUTPUT_ISO")"
WORK_DIR="$(realpath -m "$WORK_DIR")"

[[ "$WORK_DIR" != / && "$WORK_DIR" != "$REPO_ROOT" ]] || fail 'unsafe work directory'
[[ "$OUTPUT_ISO" != "$SOURCE_ISO" ]] || fail 'output ISO must differ from the source ISO'
case "$SOURCE_ISO" in "$WORK_DIR"/*) fail 'source ISO cannot be inside the work directory' ;; esac
case "$OUTPUT_ISO" in "$WORK_DIR"/*) fail 'output ISO cannot be inside the work directory' ;; esac

processors="${SQUASHFS_PROCESSORS:-$(nproc)}"
[[ "$processors" =~ ^[1-9][0-9]*$ ]] || fail 'SQUASHFS_PROCESSORS must be a positive integer'
((processors > 4)) && processors=4

ISO_MOUNT="$WORK_DIR/mnt/iso"
BASE_MOUNT="$WORK_DIR/mnt/base"
STANDARD_UPPER="$WORK_DIR/layers/standard"
STANDARD_WORK="$WORK_DIR/overlay-work/standard"
STANDARD_ROOT="$WORK_DIR/mnt/standard-root"
OMNIOS_SQUASHFS="$WORK_DIR/generated/omnios.squashfs"
OMNIOS_MOUNT="$WORK_DIR/mnt/omnios"
LIVE_UPPER="$WORK_DIR/layers/live"
LIVE_WORK="$WORK_DIR/overlay-work/live"
LIVE_ROOT="$WORK_DIR/mnt/live-root"
OMNIOS_LIVE_SQUASHFS="$WORK_DIR/generated/omnios.live.squashfs"
GENERATED="$WORK_DIR/generated"

rm -rf -- "$WORK_DIR"
mkdir -p \
  "$ISO_MOUNT" "$BASE_MOUNT" "$WORK_DIR/layers" \
  "$STANDARD_WORK" "$STANDARD_ROOT" \
  "$OMNIOS_MOUNT" "$LIVE_WORK" "$LIVE_ROOT" "$GENERATED" \
  "$(dirname -- "$OUTPUT_ISO")"

log "Mounting source image: $SOURCE_ISO"
mount_track -o loop,ro "$SOURCE_ISO" "$ISO_MOUNT"

for layer in minimal.squashfs minimal.standard.squashfs minimal.standard.live.squashfs; do
  [[ -f "$ISO_MOUNT/casper/$layer" ]] || fail "source ISO is missing casper/$layer"
done
[[ -f "$ISO_MOUNT/boot/grub/grub.cfg" ]] || fail 'source ISO is missing its GRUB configuration'
[[ -f "$ISO_MOUNT/md5sum.txt" ]] || fail 'source ISO is missing md5sum.txt'

log 'Composing the installed OmniOS filesystem'
mount_track -t squashfs -o loop,ro "$ISO_MOUNT/casper/minimal.squashfs" "$BASE_MOUNT"
unsquashfs -no-progress -d "$STANDARD_UPPER" "$ISO_MOUNT/casper/minimal.standard.squashfs"
mount_track -t overlay overlay \
  -o "lowerdir=$BASE_MOUNT,upperdir=$STANDARD_UPPER,workdir=$STANDARD_WORK,index=off" \
  "$STANDARD_ROOT"

customize_root() {
  local root="$1"
  local staging="$root/tmp/omnios-build"
  local resolver="$root/run/systemd/resolve/stub-resolv.conf"

  rm -rf -- "$staging"
  mkdir -p "$staging/scripts" "$staging/assets" "$(dirname -- "$resolver")"
  cp -a "$REPO_ROOT/assets/." "$staging/assets/"
  cp -a "$REPO_ROOT/scripts/customize.sh" "$staging/scripts/customize.sh"

  touch "$resolver"
  mount_track --rbind /dev "$root/dev"
  mount --make-rslave "$root/dev"
  mount_track -t proc proc "$root/proc"
  mount_track -t sysfs sysfs "$root/sys"
  mount_track --bind /etc/resolv.conf "$resolver"

  chroot "$root" /usr/bin/env \
    HOME=/root \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    /bin/bash /tmp/omnios-build/scripts/customize.sh

  unmount_path "$resolver"
  unmount_path "$root/sys"
  unmount_path "$root/proc"
  unmount_path "$root/dev"
  rm -rf -- "$staging"
  rm -f -- "$resolver"
}

customize_root "$STANDARD_ROOT"
# dpkg-query expands these fields; the shell must pass them literally.
# shellcheck disable=SC2016
chroot "$STANDARD_ROOT" dpkg-query -W --showformat='${Package} ${Version}\n' \
  | sort > "$GENERATED/omnios.manifest"
du --summarize --block-size=1 "$STANDARD_ROOT" | awk '{print $1}' \
  > "$GENERATED/omnios.size"

log 'Compressing the installable OmniOS filesystem'
mksquashfs "$STANDARD_ROOT" "$OMNIOS_SQUASHFS" \
  -noappend -comp xz -b 1M -Xdict-size 100% -processors "$processors"

log 'Composing the OmniOS live environment'
mount_track -t squashfs -o loop,ro "$OMNIOS_SQUASHFS" "$OMNIOS_MOUNT"
unsquashfs -no-progress -d "$LIVE_UPPER" "$ISO_MOUNT/casper/minimal.standard.live.squashfs"
mount_track -t overlay overlay \
  -o "lowerdir=$OMNIOS_MOUNT,upperdir=$LIVE_UPPER,workdir=$LIVE_WORK,index=off" \
  "$LIVE_ROOT"
customize_root "$LIVE_ROOT"
# dpkg-query expands these fields; the shell must pass them literally.
# shellcheck disable=SC2016
chroot "$LIVE_ROOT" dpkg-query -W --showformat='${Package} ${Version}\n' \
  | sort > "$GENERATED/omnios.live.manifest"
du --summarize --block-size=1 "$LIVE_ROOT" | awk '{print $1}' \
  > "$GENERATED/omnios.live.size"

log 'Compressing the OmniOS live layer'
mksquashfs "$LIVE_UPPER" "$OMNIOS_LIVE_SQUASHFS" \
  -noappend -comp xz -b 1M -Xdict-size 100% -processors "$processors"

install_size="$(cat "$GENERATED/omnios.size")"
cat > "$GENERATED/install-sources.yaml" <<EOF
- default: true
  description:
    en: OmniOS is an independent desktop experience built on the Ubuntu 24.04 LTS foundation.
  id: omnios-desktop
  locale_support: locale-only
  name:
    en: OmniOS Desktop
  path: omnios.squashfs
  size: $install_size
  type: fsimage
  variant: desktop
EOF

sed \
  -e 's/Ubuntu/OmniOS/g' \
  -e 's/minimal\.standard\.live\.squashfs/omnios.live.squashfs/g' \
  "$ISO_MOUNT/boot/grub/grub.cfg" > "$GENERATED/grub.cfg"
if [[ -f "$ISO_MOUNT/boot/grub/loopback.cfg" ]]; then
  sed \
    -e 's/Ubuntu/OmniOS/g' \
    -e 's/minimal\.standard\.live\.squashfs/omnios.live.squashfs/g' \
    "$ISO_MOUNT/boot/grub/loopback.cfg" > "$GENERATED/loopback.cfg"
fi
printf 'OmniOS 1.0 amd64\n' > "$GENERATED/disk-info"

# Rebuild the per-casper SHA-256 index. Unchanged files are hashed directly
# from the mounted source; all old minimal.* layers are intentionally omitted.
: > "$GENERATED/casper-SHA256SUMS"
while IFS= read -r -d '' file; do
  name="$(basename -- "$file")"
  case "$name" in
    minimal*|filesystem.manifest|filesystem.size|install-sources.yaml|SHA256SUMS|SHA256SUMS.gpg) continue ;;
  esac
  hash="$(sha256sum "$file" | awk '{print $1}')"
  printf '%s *%s\n' "$hash" "$name" >> "$GENERATED/casper-SHA256SUMS"
done < <(find "$ISO_MOUNT/casper" -maxdepth 1 -type f -print0)
for name in omnios.squashfs omnios.live.squashfs omnios.manifest omnios.live.manifest omnios.size omnios.live.size; do
  hash="$(sha256sum "$GENERATED/$name" | awk '{print $1}')"
  printf '%s *%s\n' "$hash" "$name" >> "$GENERATED/casper-SHA256SUMS"
done
for pair in \
  "$GENERATED/omnios.live.manifest:filesystem.manifest" \
  "$GENERATED/omnios.live.size:filesystem.size" \
  "$GENERATED/install-sources.yaml:install-sources.yaml"; do
  file="${pair%%:*}"
  name="${pair#*:}"
  hash="$(sha256sum "$file" | awk '{print $1}')"
  printf '%s *%s\n' "$hash" "$name" >> "$GENERATED/casper-SHA256SUMS"
done
sort -k2,2 -o "$GENERATED/casper-SHA256SUMS" "$GENERATED/casper-SHA256SUMS"

# Preserve valid MD5 entries for unchanged source files and add every mapped file.
awk '
  {
    path = $2
    sub(/^\*/, "", path)
    sub(/^\.\//, "", path)
    if (path ~ /^casper\/minimal/) next
    if (path ~ /^casper\/(filesystem\.(manifest|size)|install-sources\.yaml|SHA256SUMS(\.gpg)?)$/) next
    if (path == "boot/grub/grub.cfg" || path == "boot/grub/loopback.cfg") next
    if (path == ".disk/info") next
    print
  }
' "$ISO_MOUNT/md5sum.txt" > "$GENERATED/md5sum.txt"

append_md5() {
  local file="$1"
  local iso_path="$2"
  local hash
  hash="$(md5sum "$file" | awk '{print $1}')"
  printf '%s  ./%s\n' "$hash" "$iso_path" >> "$GENERATED/md5sum.txt"
}
append_md5 "$OMNIOS_SQUASHFS" casper/omnios.squashfs
append_md5 "$OMNIOS_LIVE_SQUASHFS" casper/omnios.live.squashfs
append_md5 "$GENERATED/omnios.manifest" casper/omnios.manifest
append_md5 "$GENERATED/omnios.live.manifest" casper/omnios.live.manifest
append_md5 "$GENERATED/omnios.size" casper/omnios.size
append_md5 "$GENERATED/omnios.live.size" casper/omnios.live.size
append_md5 "$GENERATED/omnios.live.manifest" casper/filesystem.manifest
append_md5 "$GENERATED/omnios.live.size" casper/filesystem.size
append_md5 "$GENERATED/install-sources.yaml" casper/install-sources.yaml
append_md5 "$GENERATED/casper-SHA256SUMS" casper/SHA256SUMS
append_md5 "$GENERATED/grub.cfg" boot/grub/grub.cfg
[[ ! -f "$GENERATED/loopback.cfg" ]] || append_md5 "$GENERATED/loopback.cfg" boot/grub/loopback.cfg
append_md5 "$GENERATED/disk-info" .disk/info
sort -k2,2 -o "$GENERATED/md5sum.txt" "$GENERATED/md5sum.txt"

log 'Rebuilding the bootable hybrid OmniOS ISO'
rm -f -- "$OUTPUT_ISO" "$OUTPUT_ISO.sha256"
xorriso_args=(
  -indev "$SOURCE_ISO"
  -outdev "$OUTPUT_ISO"
  -overwrite on
)
while IFS= read -r -d '' file; do
  xorriso_args+=( -rm "/casper/$(basename -- "$file")" )
done < <(find "$ISO_MOUNT/casper" -maxdepth 1 -type f -name 'minimal*' -print0)
xorriso_args+=(
  -rm /casper/SHA256SUMS.gpg
  -map "$OMNIOS_SQUASHFS" /casper/omnios.squashfs
  -map "$OMNIOS_LIVE_SQUASHFS" /casper/omnios.live.squashfs
  -map "$GENERATED/omnios.manifest" /casper/omnios.manifest
  -map "$GENERATED/omnios.live.manifest" /casper/omnios.live.manifest
  -map "$GENERATED/omnios.size" /casper/omnios.size
  -map "$GENERATED/omnios.live.size" /casper/omnios.live.size
  -map "$GENERATED/omnios.live.manifest" /casper/filesystem.manifest
  -map "$GENERATED/omnios.live.size" /casper/filesystem.size
  -map "$GENERATED/install-sources.yaml" /casper/install-sources.yaml
  -map "$GENERATED/casper-SHA256SUMS" /casper/SHA256SUMS
  -map "$GENERATED/grub.cfg" /boot/grub/grub.cfg
)
if [[ -f "$GENERATED/loopback.cfg" ]]; then
  xorriso_args+=( -map "$GENERATED/loopback.cfg" /boot/grub/loopback.cfg )
fi
xorriso_args+=(
  -map "$GENERATED/disk-info" /.disk/info
  -map "$GENERATED/md5sum.txt" /md5sum.txt
  -volid 'OmniOS 1.0 amd64'
  -boot_image any replay
  -commit
  -end
)
xorriso "${xorriso_args[@]}"

log 'Validating the generated ISO structure and boot records'
xorriso -indev "$OUTPUT_ISO" -report_el_torito plain >/dev/null
xorriso -indev "$OUTPUT_ISO" -find /casper/omnios.squashfs -exec report_lba -- \
  | grep -q 'omnios.squashfs' || fail 'generated ISO does not contain the install filesystem'
xorriso -indev "$OUTPUT_ISO" -find /casper/omnios.live.squashfs -exec report_lba -- \
  | grep -q 'omnios.live.squashfs' || fail 'generated ISO does not contain the live layer'
(
  cd "$(dirname -- "$OUTPUT_ISO")"
  sha256sum "$(basename -- "$OUTPUT_ISO")" > "$(basename -- "$OUTPUT_ISO").sha256"
)

log 'OmniOS ISO build complete'
ls -lh "$OUTPUT_ISO" "$OUTPUT_ISO.sha256"
