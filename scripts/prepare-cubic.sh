#!/usr/bin/env bash
# Prepare the host-side ISO and a small customization bundle for Cubic.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
OUTPUT_DIR="$REPO_ROOT/out"
DOWNLOAD_ISO=true
LAUNCH_CUBIC=false

usage() {
  cat <<'EOF'
Usage: scripts/prepare-cubic.sh [options]

Options:
  --no-download   Build the Cubic bundle without downloading an Ubuntu ISO
  --launch        Open Cubic after preparing the files
  -h, --help      Show this help
EOF
}

while (($# > 0)); do
  case "$1" in
    --no-download) DOWNLOAD_ISO=false; shift ;;
    --launch) LAUNCH_CUBIC=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'ERROR: unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
  esac
done

((EUID != 0)) || {
  printf 'ERROR: run this host helper as your desktop user, not with sudo\n' >&2
  exit 1
}

mkdir -p "$OUTPUT_DIR"
BUNDLE="$OUTPUT_DIR/omnios-cubic-bundle.tar.gz"
tar \
  --create \
  --gzip \
  --file "$BUNDLE" \
  --directory "$REPO_ROOT" \
  assets scripts/customize.sh
printf '==> Cubic customization bundle: %s\n' "$BUNDLE"

if [[ "$DOWNLOAD_ISO" == true ]]; then
  bash "$SCRIPT_DIR/download-ubuntu.sh"
fi

if [[ "$LAUNCH_CUBIC" == true ]]; then
  command -v cubic >/dev/null 2>&1 || {
    printf 'ERROR: Cubic is not installed; see README.md\n' >&2
    exit 1
  }
  printf '==> Opening Cubic\n'
  exec cubic
fi

cat <<EOF

Next:
  1. Open Cubic and select the verified ISO in $REPO_ROOT/iso/.
  2. On Cubic's Terminal page, cd to /tmp and copy in:
       $BUNDLE
  3. Run:
       tar -xzf omnios-cubic-bundle.tar.gz
       bash scripts/customize.sh
EOF
