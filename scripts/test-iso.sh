#!/usr/bin/env bash
# Headless QEMU smoke test for the OmniOS live filesystem.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
ISO_PATH="${1:-$REPO_ROOT/out/OmniOS-1.0-amd64.iso}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-600}"
BOOT_MEMORY="${BOOT_MEMORY:-4096}"
BOOT_CPUS="${BOOT_CPUS:-2}"
BOOT_LOG="${BOOT_LOG:-$REPO_ROOT/out/qemu-boot.log}"
TEST_DIR=''

usage() {
  cat <<'EOF'
Usage: scripts/test-iso.sh [ISO]

Boot the OmniOS kernel and initrd in headless QEMU, mount the live filesystem
from ISO, and require systemd to reach multi-user or graphical target.

Environment:
  BOOT_TIMEOUT  Maximum boot time in seconds (default: 600)
  BOOT_MEMORY   Guest RAM in MiB (default: 4096)
  BOOT_CPUS     Virtual CPU count (default: 2)
  BOOT_LOG      Serial log path (default: out/qemu-boot.log)
EOF
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

# Invoked indirectly by the EXIT trap.
# shellcheck disable=SC2329
cleanup() {
  [[ -z "$TEST_DIR" ]] || rm -rf -- "$TEST_DIR"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ "${1:-}" == -h || "${1:-}" == --help ]]; then
  usage
  exit 0
fi
(($# <= 1)) || fail 'only one ISO path may be supplied'

for value_name in BOOT_TIMEOUT BOOT_MEMORY BOOT_CPUS; do
  value="${!value_name}"
  [[ "$value" =~ ^[1-9][0-9]*$ ]] || fail "$value_name must be a positive integer"
done

for command_name in qemu-system-x86_64 timeout xorriso; do
  command -v "$command_name" >/dev/null 2>&1 || fail "missing required command: $command_name"
done

[[ -f "$ISO_PATH" ]] || fail "ISO not found: $ISO_PATH"
ISO_PATH="$(realpath "$ISO_PATH")"
BOOT_LOG="$(realpath -m "$BOOT_LOG")"
mkdir -p "$(dirname -- "$BOOT_LOG")"
TEST_DIR="$(mktemp -d)"

printf '==> Extracting the OmniOS kernel and initrd\n'
xorriso -osirrox on -indev "$ISO_PATH" \
  -extract /casper/vmlinuz "$TEST_DIR/vmlinuz" \
  -extract /casper/initrd "$TEST_DIR/initrd" \
  -end >/dev/null

[[ -s "$TEST_DIR/vmlinuz" ]] || fail 'the ISO does not contain /casper/vmlinuz'
[[ -s "$TEST_DIR/initrd" ]] || fail 'the ISO does not contain /casper/initrd'

accel_args=(-accel "tcg,thread=multi" -cpu max)
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
  accel_args=(-accel kvm -cpu host)
fi

printf '==> Booting OmniOS in headless QEMU (timeout: %ss)\n' "$BOOT_TIMEOUT"
set +e
timeout --signal=TERM --kill-after=30 "$BOOT_TIMEOUT" \
  qemu-system-x86_64 \
    "${accel_args[@]}" \
    -machine pc \
    -smp "$BOOT_CPUS" \
    -m "$BOOT_MEMORY" \
    -kernel "$TEST_DIR/vmlinuz" \
    -initrd "$TEST_DIR/initrd" \
    -append 'boot=casper layerfs-path=minimal.standard.live.squashfs noprompt noeject hostname=omnios username=omnios console=tty0 console=ttyS0,115200n8 systemd.unit=multi-user.target systemd.show_status=1 rd.systemd.show_status=1 loglevel=6' \
    -drive "file=$ISO_PATH,media=cdrom,readonly=on,format=raw" \
    -device virtio-rng-pci \
    -nic none \
    -display none \
    -monitor none \
    -serial stdio \
    -no-reboot \
  2>&1 | tee "$BOOT_LOG"
qemu_status=${PIPESTATUS[0]}
set -e

# A timeout is expected after the live system reaches its target and waits at a
# login prompt. Any success marker proves that Casper mounted our custom layers
# and handed control to systemd.
if grep -Eaq \
  'Reached target .*Multi-User|Reached target .*Graphical|Started .*Getty|OmniOS.*login:' \
  "$BOOT_LOG"; then
  printf '==> OmniOS live boot smoke test passed\n'
  exit 0
fi

printf 'QEMU exit status: %s\n' "$qemu_status" >&2
printf '%s\n' '--- Last 200 boot-log lines ---' >&2
tail -n 200 "$BOOT_LOG" >&2
fail "OmniOS did not reach a boot-complete target within ${BOOT_TIMEOUT}s"
