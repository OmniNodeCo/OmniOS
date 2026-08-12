#!/usr/bin/env bash
# Additional file and configuration changes run by scripts/customize.sh.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ASSETS_DIR="${OMNIOS_ASSETS_DIR:-$SCRIPT_DIR}"

((EUID == 0)) || {
  printf 'ERROR: customize-extra.sh must run as root inside Cubic\n' >&2
  exit 1
}

printf '==> Installing OmniOS MOTD\n'
install -Dm0644 "$ASSETS_DIR/motd" /etc/motd

# Add other file-based customizations here. For example:
# install -Dm0644 "$ASSETS_DIR/wallpaper.jpg" \
#   /usr/share/backgrounds/omnios-wallpaper.jpg

printf '==> Extra customizations complete\n'
