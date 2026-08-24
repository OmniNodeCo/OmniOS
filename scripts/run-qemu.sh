#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
DEPLOY_DIR="${DEPLOY_DIR:-$ROOT/build/tmp/deploy/images/genericx86-64}"
IMAGE="${1:-}"
[[ -n "$IMAGE" ]] || IMAGE="$(find "$DEPLOY_DIR" -maxdepth 1 -type f -name 'omnios-plasma-image-genericx86-64*.wic.zst' -print 2>/dev/null | sort | tail -1 || true)"
[[ -f "$IMAGE" ]] || { echo 'ERROR: no OmniOS .wic.zst image found' >&2; exit 1; }
for x in qemu-system-x86_64 zstd; do command -v "$x" >/dev/null || { echo "ERROR: missing $x" >&2; exit 1; }; done
mkdir -p "$ROOT/out"; RAW="$ROOT/out/OmniOS-1.0-x86_64.wic"; zstd -d -f "$IMAGE" -o "$RAW"
accel=(-accel 'tcg,thread=multi' -cpu max); [[ -r /dev/kvm && -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
firmware=()
if [[ "${QEMU_UEFI:-0}" == 1 ]]; then
 code=; vars=
 if [[ -r /usr/share/OVMF/OVMF_CODE_4M.fd && -r /usr/share/OVMF/OVMF_VARS_4M.fd ]]; then code=/usr/share/OVMF/OVMF_CODE_4M.fd; vars=/usr/share/OVMF/OVMF_VARS_4M.fd
 elif [[ -r /usr/share/OVMF/OVMF_CODE.fd && -r /usr/share/OVMF/OVMF_VARS.fd ]]; then code=/usr/share/OVMF/OVMF_CODE.fd; vars=/usr/share/OVMF/OVMF_VARS.fd; fi
 [[ -n "$code" ]] || { echo 'ERROR: OVMF required' >&2; exit 1; }; cp -f "$vars" "$ROOT/out/OVMF_VARS.fd"
 firmware=(-drive "if=pflash,format=raw,readonly=on,file=$code" -drive "if=pflash,format=raw,file=$ROOT/out/OVMF_VARS.fd")
fi
exec qemu-system-x86_64 "${accel[@]}" -machine q35 "${firmware[@]}" -smp "${QEMU_CPUS:-4}" -m "${QEMU_MEMORY:-8192}" \
 -drive "file=$RAW,format=raw,if=virtio" -device virtio-vga -device qemu-xhci -device usb-tablet -nic user,model=virtio-net-pci -boot order=c
