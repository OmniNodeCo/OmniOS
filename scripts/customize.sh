#!/usr/bin/env bash
# Apply OmniOS packages and branding inside Cubic's Terminal environment.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
ASSETS_DIR="$REPO_ROOT/assets"
PACKAGES_FILE="${PACKAGES_FILE:-$ASSETS_DIR/packages.txt}"
DRY_RUN=false
SKIP_PACKAGES=false
SKIP_BRANDING=false
KEEP_APT_CACHE=false
ALLOW_HOST=false
POLICY_CREATED=false

usage() {
  cat <<'EOF'
Usage: scripts/customize.sh [options]

Run this script as root on Cubic's Terminal page.

Options:
  --packages FILE    Read packages from FILE
  --skip-packages    Do not install additional packages
  --skip-branding    Do not install OmniOS identity or desktop branding
  --keep-apt-cache   Keep APT indexes and package archives in the image
  --dry-run          Validate files and display the plan without changing files
  --allow-host       Permit a real run outside a chroot (unsafe)
  -h, --help         Show this help
EOF
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

log() {
  printf '==> %s\n' "$*"
}

is_chroot() {
  [[ -r /proc/1/root/. ]] || return 1
  [[ "$(stat -Lc '%d:%i' /)" != "$(stat -Lc '%d:%i' /proc/1/root/.)" ]]
}

cleanup() {
  if [[ "$POLICY_CREATED" == true ]]; then
    rm -f /usr/sbin/policy-rc.d
    POLICY_CREATED=false
  fi
}

while (($# > 0)); do
  case "$1" in
    --packages)
      (($# >= 2)) || fail '--packages requires a file path'
      PACKAGES_FILE="$2"
      shift 2
      ;;
    --skip-packages) SKIP_PACKAGES=true; shift ;;
    --skip-branding) SKIP_BRANDING=true; shift ;;
    --keep-apt-cache) KEEP_APT_CACHE=true; shift ;;
    --dry-run) DRY_RUN=true; shift ;;
    --allow-host) ALLOW_HOST=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown option: $1" ;;
  esac
done

packages=()
if [[ "$SKIP_PACKAGES" == false ]]; then
  [[ -r "$PACKAGES_FILE" ]] || fail "package list is not readable: $PACKAGES_FILE"
  line_number=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    ((line_number += 1))
    line="${line%%#*}"
    read -r -a fields <<< "$line"
    for package in "${fields[@]}"; do
      [[ "$package" =~ ^[a-z0-9][a-z0-9+.-]*(:[a-z0-9]+)?$ ]] \
        || fail "invalid package '$package' at $PACKAGES_FILE:$line_number"
      packages+=("$package")
    done
  done < "$PACKAGES_FILE"
  ((${#packages[@]} > 0)) || fail "package list is empty: $PACKAGES_FILE"
fi

branding_files=(
  "$ASSETS_DIR/os-release"
  "$ASSETS_DIR/lsb-release"
  "$ASSETS_DIR/omnios-release"
  "$ASSETS_DIR/motd"
  "$ASSETS_DIR/omnios-default.svg"
  "$ASSETS_DIR/omnios-wallpapers.xml"
  "$ASSETS_DIR/90-omnios.gschema.override"
)
if [[ "$SKIP_BRANDING" == false ]]; then
  for file in "${branding_files[@]}"; do
    [[ -r "$file" ]] || fail "branding asset is not readable: $file"
  done
fi

if [[ "$DRY_RUN" == true ]]; then
  log 'Dry run; no packages or files will be changed'
  if [[ "$SKIP_PACKAGES" == false ]]; then
    printf 'Packages (%d): %s\n' "${#packages[@]}" "${packages[*]}"
  else
    printf 'Packages: skipped\n'
  fi
  if [[ "$SKIP_BRANDING" == false ]]; then
    printf 'Branding assets: %d files\n' "${#branding_files[@]}"
  else
    printf 'Branding: skipped\n'
  fi
  exit 0
fi

((EUID == 0)) || fail "Cubic's Terminal runs as root; execute this script there without sudo"
command -v apt-get >/dev/null 2>&1 || fail 'apt-get is required'
if [[ "$ALLOW_HOST" == false ]] && ! is_chroot; then
  fail "no chroot detected; refusing to modify the host (run this in Cubic)"
fi

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Package post-install hooks must not try to start services in Cubic's static
# filesystem. Preserve an existing policy supplied by Cubic.
if [[ ! -e /usr/sbin/policy-rc.d ]]; then
  cat > /usr/sbin/policy-rc.d <<'EOF'
#!/bin/sh
exit 101
EOF
  chmod 0755 /usr/sbin/policy-rc.d
  POLICY_CREATED=true
fi

export DEBIAN_FRONTEND=noninteractive
export NEEDRESTART_MODE=a

if [[ "$SKIP_PACKAGES" == false ]]; then
  log 'Refreshing Ubuntu package indexes'
  apt-get update
  log "Installing ${#packages[@]} additional packages"
  apt-get install --yes "${packages[@]}"
else
  log 'Package installation skipped'
fi

if [[ "$SKIP_BRANDING" == false ]]; then
  log 'Installing OmniOS identity and desktop branding'
  # /etc/os-release takes precedence over Ubuntu's /usr/lib/os-release and is
  # not replaced by base-files package updates.
  rm -f /etc/os-release
  install -Dm0644 "$ASSETS_DIR/os-release" /etc/os-release
  install -Dm0644 "$ASSETS_DIR/lsb-release" /etc/lsb-release
  install -Dm0644 "$ASSETS_DIR/omnios-release" /etc/omnios-release
  install -Dm0644 "$ASSETS_DIR/motd" /etc/motd
  install -Dm0644 "$ASSETS_DIR/omnios-default.svg" \
    /usr/share/backgrounds/omnios/omnios-default.svg
  install -Dm0644 "$ASSETS_DIR/omnios-wallpapers.xml" \
    /usr/share/gnome-background-properties/omnios-wallpapers.xml
  install -Dm0644 "$ASSETS_DIR/90-omnios.gschema.override" \
    /usr/share/glib-2.0/schemas/90-omnios.gschema.override
  if command -v glib-compile-schemas >/dev/null 2>&1; then
    glib-compile-schemas /usr/share/glib-2.0/schemas
  fi
else
  log 'Branding installation skipped'
fi

if [[ -r "$ASSETS_DIR/customize-extra.sh" ]]; then
  log 'Running optional extra customizations'
  OMNIOS_ASSETS_DIR="$ASSETS_DIR" bash "$ASSETS_DIR/customize-extra.sh"
fi

if [[ "$KEEP_APT_CACHE" == false ]]; then
  log 'Cleaning APT data to reduce ISO size'
  apt-get clean
  rm -rf /var/lib/apt/lists/*
fi

cleanup
trap - EXIT HUP INT TERM
log 'Customization complete; return to Cubic and click Next'
