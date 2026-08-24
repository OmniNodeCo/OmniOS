#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
ISO="${1:-$ROOT/out/OmniOS-1.0-amd64.iso}"
[[ -f "$ISO" ]] || { echo "ERROR: ISO not found: $ISO" >&2; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo 'ERROR: qemu-system-x86_64 is required.' >&2; exit 1; }

accel=(-accel 'tcg,thread=multi' -cpu max)
[[ -r /dev/kvm && -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
firmware=()
if [[ "${QEMU_UEFI:-0}" == 1 ]]; then
    code='' vars=''
    if [[ -r /usr/share/OVMF/OVMF_CODE_4M.fd && -r /usr/share/OVMF/OVMF_VARS_4M.fd ]]; then
        code=/usr/share/OVMF/OVMF_CODE_4M.fd; vars=/usr/share/OVMF/OVMF_VARS_4M.fd
    elif [[ -r /usr/share/OVMF/OVMF_CODE.fd && -r /usr/share/OVMF/OVMF_VARS.fd ]]; then
        code=/usr/share/OVMF/OVMF_CODE.fd; vars=/usr/share/OVMF/OVMF_VARS.fd
    fi
    [[ -n "$code" ]] || { echo 'ERROR: OVMF firmware is required for UEFI mode.' >&2; exit 1; }
    mkdir -p "$ROOT/out"
    cp -f "$vars" "$ROOT/out/OVMF_VARS.fd"
    firmware=(-drive "if=pflash,format=raw,readonly=on,file=$code" -drive "if=pflash,format=raw,file=$ROOT/out/OVMF_VARS.fd")
fi

exec qemu-system-x86_64 "${accel[@]}" -machine q35 "${firmware[@]}" \
    -smp "${QEMU_CPUS:-4}" -m "${QEMU_MEMORY:-6144}" \
    -cdrom "$ISO" -boot d \
    -device virtio-vga -device qemu-xhci -device usb-tablet \
    -nic user,model=virtio-net-pci
