#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
ISO="${1:-$ROOT/out/OmniOS-1.0-amd64.iso}"
[[ -f "$ISO" ]] || { echo "ERROR: ISO not found: $ISO" >&2; exit 1; }
for command_name in qemu-system-x86_64 timeout; do
    command -v "$command_name" >/dev/null || { echo "ERROR: missing $command_name" >&2; exit 1; }
done

mkdir -p "$ROOT/out"
mode=bios
[[ "${QEMU_UEFI:-0}" == 1 ]] && mode=uefi
LOG="$ROOT/out/OmniOS-smoke-$mode.log"
accel=(-accel 'tcg,thread=multi' -cpu max)
[[ -r /dev/kvm && -w /dev/kvm ]] && accel=(-enable-kvm -cpu host)
firmware=()
if [[ "$mode" == uefi ]]; then
    code='' vars=''
    if [[ -r /usr/share/OVMF/OVMF_CODE_4M.fd && -r /usr/share/OVMF/OVMF_VARS_4M.fd ]]; then
        code=/usr/share/OVMF/OVMF_CODE_4M.fd; vars=/usr/share/OVMF/OVMF_VARS_4M.fd
    elif [[ -r /usr/share/OVMF/OVMF_CODE.fd && -r /usr/share/OVMF/OVMF_VARS.fd ]]; then
        code=/usr/share/OVMF/OVMF_CODE.fd; vars=/usr/share/OVMF/OVMF_VARS.fd
    fi
    [[ -n "$code" ]] || { echo 'ERROR: OVMF is required.' >&2; exit 1; }
    cp -f "$vars" "$ROOT/out/OVMF_VARS-smoke.fd"
    firmware=(-drive "if=pflash,format=raw,readonly=on,file=$code" -drive "if=pflash,format=raw,file=$ROOT/out/OVMF_VARS-smoke.fd")
fi

set +e
timeout --signal=TERM --kill-after=15 "${SMOKE_TIMEOUT:-600}" \
    qemu-system-x86_64 "${accel[@]}" -machine q35 "${firmware[@]}" \
    -smp "${QEMU_CPUS:-4}" -m "${QEMU_MEMORY:-4096}" \
    -cdrom "$ISO" -boot d -nic user,model=virtio-net-pci \
    -display none -monitor none -serial stdio -no-reboot >"$LOG" 2>&1
status=$?
set -e

if grep -Eq 'OmniOS 1\.0|Reached target (Graphical Interface|Multi-User System)|Started .*Display Manager|Started sddm' "$LOG"; then
    echo "PASS: OmniOS $mode live boot reached userspace (QEMU status $status)."
    exit 0
fi

echo "FAIL: no OmniOS userspace marker appeared ($mode, QEMU status $status)." >&2
tail -n 120 "$LOG" >&2
exit 1
