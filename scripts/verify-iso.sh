#!/usr/bin/env bash
# Verify the built OmniOS ISO against the checksum produced by the build.
#
# The version and therefore every output filename comes from version.txt, so
# bumping that file is all it takes to build and verify a new release. Nothing
# here, and nothing in the CI workflow that calls it, hardcodes a version.
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
VERSION="$("$ROOT/scripts/read-version.sh")"
ARCH="${OMNIOS_ARCH:-amd64}"
BASENAME="OmniOS-${VERSION}-${ARCH}"

cd "$OUT_DIR" || {
    echo "ERROR: no build output directory: $OUT_DIR" >&2
    exit 1
}

iso="$BASENAME.iso"
checksum="$BASENAME.iso.sha256"

if [[ ! -f "$iso" || ! -f "$checksum" ]]; then
    echo "ERROR: expected $iso and $checksum in $OUT_DIR" >&2
    echo "version.txt selects the name, and it currently contains $VERSION." >&2
    echo "Files actually present:" >&2
    find . -maxdepth 1 -type f -printf '  %f\n' | sort >&2
    exit 1
fi

sha256sum --check "$checksum"
printf 'Verified %s (%s)\n' "$iso" "$(sha256sum "$iso" | cut -c1-16)…"
