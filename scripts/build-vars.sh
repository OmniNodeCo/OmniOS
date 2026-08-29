#!/usr/bin/env bash
# Print every version-derived name used by the build and release workflows.
#
# version.txt is the single source of truth. CI reads these values instead of
# computing them inline, so a stale workflow cannot disagree with the version
# the build actually produced.
#
# Usage:
#   scripts/build-vars.sh              # KEY=VALUE lines
#   scripts/build-vars.sh --github-env # append to $GITHUB_ENV
#   scripts/build-vars.sh <key>        # print one value
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION="$("$ROOT/scripts/read-version.sh")"
ARCH="${OMNIOS_ARCH:-amd64}"
ARTIFACT="OmniOS-${VERSION}-${ARCH}"

emit() {
    printf 'OMNIOS_VERSION=%s\n' "$VERSION"
    printf 'OMNIOS_ARCH=%s\n' "$ARCH"
    printf 'OMNIOS_ARTIFACT=%s\n' "$ARTIFACT"
    printf 'OMNIOS_ISO=%s.iso\n' "$ARTIFACT"
    printf 'OMNIOS_ISO_SHA256=%s.iso.sha256\n' "$ARTIFACT"
    printf 'OMNIOS_RELEASE_TAG=v%s\n' "$VERSION"
}

case "${1:-}" in
    '')
        emit
        ;;
    --github-env)
        [[ -n "${GITHUB_ENV:-}" ]] || {
            echo 'ERROR: GITHUB_ENV is not set' >&2
            exit 1
        }
        emit >> "$GITHUB_ENV"
        emit
        ;;
    --*)
        echo "ERROR: unknown option: $1" >&2
        exit 2
        ;;
    *)
        value="$(emit | sed -n "s/^$1=//p")"
        [[ -n "$value" ]] || { echo "ERROR: unknown key: $1" >&2; exit 2; }
        printf '%s\n' "$value"
        ;;
esac
