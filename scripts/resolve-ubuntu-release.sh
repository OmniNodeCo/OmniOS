#!/usr/bin/env bash
# Resolve an Ubuntu Desktop ISO name and checksum from the official release index.
set -Eeuo pipefail

RELEASE="${UBUNTU_RELEASE:-noble}"
BASE_URL="${UBUNTU_BASE_URL:-https://releases.ubuntu.com/$RELEASE}"
REQUESTED_VERSION="${UBUNTU_VERSION:-latest}"
CHECKSUMS="$(mktemp)"

cleanup() {
  rm -f "$CHECKSUMS"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case "$REQUESTED_VERSION" in
  latest) ;;
  *)
    [[ "$REQUESTED_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
      printf 'ERROR: UBUNTU_VERSION must be "latest" or look like 24.04.4\n' >&2
      exit 1
    }
    ;;
esac

if command -v curl >/dev/null 2>&1; then
  curl --fail --location --silent --show-error "$BASE_URL/SHA256SUMS" > "$CHECKSUMS"
elif command -v wget >/dev/null 2>&1; then
  wget --quiet "$BASE_URL/SHA256SUMS" -O "$CHECKSUMS"
else
  printf 'ERROR: curl or wget is required\n' >&2
  exit 1
fi

if [[ "$REQUESTED_VERSION" == latest ]]; then
  VERSION="$(
    sed -nE 's/^[[:xdigit:]]{64} [* ]ubuntu-([0-9]+\.[0-9]+\.[0-9]+)-desktop-amd64\.iso$/\1/p' "$CHECKSUMS" \
      | sort -V \
      | tail -n 1
  )"
else
  VERSION="$REQUESTED_VERSION"
fi

[[ -n "$VERSION" ]] || {
  printf 'ERROR: no Ubuntu Desktop AMD64 ISO was found at %s\n' "$BASE_URL" >&2
  exit 1
}

ISO="ubuntu-${VERSION}-desktop-amd64.iso"
SHA256="$(
  awk -v iso="$ISO" '
    {
      name = $2
      sub(/^\*/, "", name)
      if (name == iso) {
        print $1
        exit
      }
    }
  ' "$CHECKSUMS"
)"

[[ "$SHA256" =~ ^[[:xdigit:]]{64}$ ]] || {
  printf 'ERROR: %s is not published at %s\n' "$ISO" "$BASE_URL" >&2
  exit 1
}

# This key=value output can be appended directly to GITHUB_OUTPUT.
printf 'version=%s\n' "$VERSION"
printf 'iso=%s\n' "$ISO"
printf 'sha256=%s\n' "${SHA256,,}"
printf 'url=%s/%s\n' "$BASE_URL" "$ISO"
