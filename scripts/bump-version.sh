#!/usr/bin/env bash
# Increment the patch component of version.txt.
#
# APT only offers an upgrade when the candidate version is higher than what is
# installed, so publishing a rebuilt package under the same version changes
# nothing on an installed system. The APT repository workflow calls this so a
# change to the packaged files becomes an update people actually receive.
#
#   2026.1    -> 2026.1.1
#   2026.1.1  -> 2026.1.2
#   2026.1.9  -> 2026.1.10
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION_FILE="${1:-$ROOT/version.txt}"

current="$("$ROOT/scripts/read-version.sh" "$VERSION_FILE")"

case "$current" in
    *.*.*)
        series="${current%.*}"
        patch="${current##*.}"
        ;;
    *)
        # A two-part version such as 2026.1 becomes 2026.1.1.
        series="$current"
        patch=0
        ;;
esac

# Base 10 explicitly: 08 and 09 are not valid octal and would fail otherwise.
next="$series.$((10#$patch + 1))"

printf '%s\n' "$next" > "$VERSION_FILE"

# Re-read through the validator so an invalid result cannot be written out.
"$ROOT/scripts/read-version.sh" "$VERSION_FILE" >/dev/null

printf '%s\n' "$next"
