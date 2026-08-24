#!/usr/bin/env bash
set -Eeuo pipefail

# Compatibility for the previously installed Yocto workflow. OmniOS now uses
# Debian's signed APT repositories and does not build RAUC bundles.
echo 'RAUC signing is not used by the Debian ISO build; continuing.'
