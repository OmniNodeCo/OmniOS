#!/usr/bin/env bash
# Cubic owns ISO extraction and repackaging; this helper opens its GUI.
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: bash scripts/build-iso.sh

Open Cubic to build the customized Ubuntu ISO. Cubic has no unattended build
CLI, so select the source ISO and complete the wizard in the GUI. On Cubic's
Terminal page, copy this repository's scripts/ and assets/ directories into
the chroot and run:

  bash scripts/customize.sh
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

(($# == 0)) || {
  printf 'ERROR: Cubic does not accept ISO build arguments\n' >&2
  usage >&2
  exit 2
}

((EUID != 0)) || {
  printf 'ERROR: launch Cubic as your desktop user, not with sudo\n' >&2
  exit 1
}

command -v cubic >/dev/null 2>&1 || {
  cat >&2 <<'EOF'
ERROR: Cubic is not installed.

Install it on an Ubuntu desktop host with:
  sudo add-apt-repository universe
  sudo add-apt-repository ppa:cubic-wizard/release
  sudo apt update
  sudo apt install --no-install-recommends cubic
EOF
  exit 1
}

printf 'Cubic will open now. See README.md for the complete workflow.\n'
exec cubic
