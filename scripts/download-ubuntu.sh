#!/usr/bin/env bash
# Download and verify the current Ubuntu 24.04 LTS desktop point release.
set -Eeuo pipefail

RELEASE="${UBUNTU_RELEASE:-noble}"
BASE_URL="${UBUNTU_BASE_URL:-https://releases.ubuntu.com/$RELEASE}"
REQUESTED_VERSION="${UBUNTU_VERSION:-}"
ISO_DIR="${ISO_DIR:-iso}"

if [[ -n "$REQUESTED_VERSION" ]] && [[ ! "$REQUESTED_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  printf 'ERROR: UBUNTU_VERSION must look like 24.04.4\n' >&2
  exit 1
fi

CHECKSUMS="$(mktemp)"
trap 'rm -f "$CHECKSUMS"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

command -v wget >/dev/null 2>&1 || {
  printf 'ERROR: wget is required\n' >&2
  exit 1
}
command -v sha256sum >/dev/null 2>&1 || {
  printf 'ERROR: sha256sum is required\n' >&2
  exit 1
}

printf '==> Reading release checksums from %s\n' "$BASE_URL/SHA256SUMS"
wget -q "$BASE_URL/SHA256SUMS" -O "$CHECKSUMS"

if [[ -n "$REQUESTED_VERSION" ]]; then
  UBUNTU_ISO="ubuntu-${REQUESTED_VERSION}-desktop-amd64.iso"
else
  UBUNTU_ISO="$(
    sed -nE 's/^[[:xdigit:]]{64} [* ](ubuntu-[0-9]+\.[0-9]+\.[0-9]+-desktop-amd64\.iso)$/\1/p' "$CHECKSUMS" \
      | sort -V \
      | tail -n 1
  )"
  [[ -n "$UBUNTU_ISO" ]] || {
    printf 'ERROR: no Ubuntu desktop AMD64 image was found in SHA256SUMS\n' >&2
    exit 1
  }
fi

CHECKSUM_LINE="$(grep -E "^[[:xdigit:]]{64} [* ]${UBUNTU_ISO//./\\.}$" "$CHECKSUMS" || true)"
[[ -n "$CHECKSUM_LINE" ]] || {
  printf 'ERROR: %s is not published at %s\n' "$UBUNTU_ISO" "$BASE_URL" >&2
  exit 1
}

mkdir -p "$ISO_DIR"
ISO_PATH="$ISO_DIR/$UBUNTU_ISO"

if [[ -f "$ISO_PATH" ]]; then
  printf '==> Verifying existing image: %s\n' "$ISO_PATH"
else
  printf '==> Downloading %s\n' "$UBUNTU_ISO"
  wget -c "$BASE_URL/$UBUNTU_ISO" -O "$ISO_PATH"
fi

if ! printf '%s\n' "$CHECKSUM_LINE" | (cd "$ISO_DIR" && sha256sum --check -); then
  printf 'ERROR: checksum verification failed; removing %s\n' "$ISO_PATH" >&2
  rm -f "$ISO_PATH"
  exit 1
fi

printf '==> Ubuntu image is ready: %s\n' "$ISO_PATH"
