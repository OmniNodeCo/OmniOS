#!/usr/bin/env bash
# Download and verify an Ubuntu Desktop ISO, reusing a valid local copy.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
OUTPUT_DIR="${ISO_DIR:-$SCRIPT_DIR/../iso}"
REQUESTED_VERSION="${UBUNTU_VERSION:-latest}"
METADATA_ONLY=false

usage() {
  cat <<'EOF'
Usage: scripts/download-ubuntu.sh [options]

Options:
  --version VERSION   Ubuntu point release, or "latest" (default: latest)
  --output-dir DIR    ISO cache directory (default: ./iso)
  --metadata-only     Resolve release metadata without downloading the ISO
  -h, --help          Show this help
EOF
}

while (($# > 0)); do
  case "$1" in
    --version)
      (($# >= 2)) || { printf 'ERROR: --version requires a value\n' >&2; exit 2; }
      REQUESTED_VERSION="$2"
      shift 2
      ;;
    --output-dir)
      (($# >= 2)) || { printf 'ERROR: --output-dir requires a path\n' >&2; exit 2; }
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --metadata-only)
      METADATA_ONLY=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'ERROR: unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

VERSION=''
ISO=''
SHA256=''
URL=''
if ! RELEASE_METADATA="$(
  UBUNTU_VERSION="$REQUESTED_VERSION" \
    bash "$SCRIPT_DIR/resolve-ubuntu-release.sh"
)"; then
  exit 1
fi
while IFS='=' read -r key value; do
  case "$key" in
    version) VERSION="$value" ;;
    iso) ISO="$value" ;;
    sha256) SHA256="$value" ;;
    url) URL="$value" ;;
  esac
done <<< "$RELEASE_METADATA"

[[ -n "$VERSION" && -n "$ISO" && -n "$SHA256" && -n "$URL" ]] || {
  printf 'ERROR: incomplete Ubuntu release metadata\n' >&2
  exit 1
}

if [[ "$METADATA_ONLY" == true ]]; then
  printf 'version=%s\niso=%s\nsha256=%s\nurl=%s\n' "$VERSION" "$ISO" "$SHA256" "$URL"
  exit 0
fi

mkdir -p "$OUTPUT_DIR"
ISO_PATH="$OUTPUT_DIR/$ISO"
PART_PATH="$ISO_PATH.part"

verify_iso() {
  [[ -f "$ISO_PATH" ]] || return 1
  printf '%s *%s\n' "$SHA256" "$ISO" | (cd "$OUTPUT_DIR" && sha256sum --check --status -)
}

if verify_iso; then
  printf '==> Using verified cached ISO: %s\n' "$ISO_PATH"
  exit 0
fi

if [[ -f "$ISO_PATH" ]]; then
  printf '==> Removing invalid cached ISO: %s\n' "$ISO_PATH"
  rm -f "$ISO_PATH"
fi

printf '==> Downloading Ubuntu %s Desktop AMD64\n' "$VERSION"
if command -v wget >/dev/null 2>&1; then
  wget --continue "$URL" -O "$PART_PATH"
elif command -v curl >/dev/null 2>&1; then
  curl --fail --location --continue-at - "$URL" -o "$PART_PATH"
else
  printf 'ERROR: curl or wget is required\n' >&2
  exit 1
fi
mv -f "$PART_PATH" "$ISO_PATH"

if ! verify_iso; then
  printf 'ERROR: SHA-256 verification failed for %s\n' "$ISO_PATH" >&2
  rm -f "$ISO_PATH"
  exit 1
fi

printf '==> Verified ISO ready: %s\n' "$ISO_PATH"
