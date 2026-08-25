#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
(( $# <= 1 )) || {
    echo 'Usage: scripts/read-version.sh [version-file]' >&2
    exit 2
}
VERSION_FILE="${1:-$ROOT/version.txt}"
[[ -f "$VERSION_FILE" ]] || {
    echo "ERROR: OmniOS version file not found: $VERSION_FILE" >&2
    exit 1
}

version="$(tr -d '[:space:]' < "$VERSION_FILE")"
[[ "$version" =~ ^[0-9]{4}\.[0-9]+(\.[0-9]+)?$ ]] || {
    echo "ERROR: version.txt must contain one version such as 2026.1" >&2
    exit 1
}

printf '%s\n' "$version"
