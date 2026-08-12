#!/usr/bin/env bash
# Add project-specific customization commands here. This script is executed
# inside Cubic after packages and core OmniOS branding have been installed.
set -Eeuo pipefail

ASSETS_DIR="${OMNIOS_ASSETS_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)}"

# Example:
# install -Dm0644 "$ASSETS_DIR/example.conf" /etc/example.conf

printf '==> No additional customizations configured (assets: %s)\n' "$ASSETS_DIR"
