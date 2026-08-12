#!/usr/bin/env bash
# Apply OmniOS customizations from Cubic's Terminal page.
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
PACKAGES_FILE="${PACKAGES_FILE:-$REPO_ROOT/assets/packages.txt}"
EXTRA_SCRIPT="${EXTRA_SCRIPT:-$REPO_ROOT/assets/customize-extra.sh}"
DRY_RUN=false
SKIP_PACKAGES=false
SKIP_EXTRA=false
KEEP_APT_CACHE=false
ALLOW_HOST=false
POLICY_CREATED=false

usage() {
  cat <<'EOF'
Usage: bash scripts/customize.sh [options]

Run OmniOS customizations inside the chroot on Cubic's Terminal page.

Options:
  --packages FILE       Use a different package list.
  --skip-packages       Do not install packages.
  --skip-extra          Do not run assets/customize-extra.sh.
  --keep-apt-cache      Keep downloaded package indexes and archives.
  --dry-run             Validate inputs and show the planned changes only.
  --allow-host          Allow a real run outside a chroot (unsafe).
  -h, --help            Show this help.

The script intentionally refuses to modify the host OS. Use --allow-host only
for a disposable build container; normal Cubic use never needs this option.
EOF
}

log() {
  printf '==> %s\n' "$*"
}

fail() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

is_chroot() {
  # This is the same root-inode comparison Cubic uses to protect the host.
  [[ -r /proc/1/root/. ]] || return 1
  [[ "$(stat -Lc '%d:%i' /)" != "$(stat -Lc '%d:%i' /proc/1/root/.)" ]]
}

cleanup() {
  if [[ "$POLICY_CREATED" == true ]]; then
    rm -f /usr/sbin/policy-rc.d
    POLICY_CREATED=false
  fi
}

print_command() {
  printf '    '
  printf '%q ' "$@"
  printf '\n'
}

while (($# > 0)); do
  case "$1" in
    --packages)
      (($# >= 2)) || fail "--packages requires a file path"
      PACKAGES_FILE="$2"
      shift 2
      ;;
    --skip-packages)
      SKIP_PACKAGES=true
      shift
      ;;
    --skip-extra)
      SKIP_EXTRA=true
      shift
      ;;
    --keep-apt-cache)
      KEEP_APT_CACHE=true
      shift
      ;;
    --dry-run)
      DRY_RUN=true
      shift
      ;;
    --allow-host)
      ALLOW_HOST=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1 (use --help for usage)"
      ;;
  esac
done

packages=()
if [[ "$SKIP_PACKAGES" == false ]]; then
  [[ -r "$PACKAGES_FILE" ]] || fail "package list is not readable: $PACKAGES_FILE"

  line_number=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    ((line_number += 1))
    line="${line%%#*}"

    # A package file may contain one or more package names per line.
    read -r -a fields <<< "$line"
    for package in "${fields[@]}"; do
      if [[ ! "$package" =~ ^[a-z0-9][a-z0-9+.-]*(:[a-z0-9]+)?(=[^[:space:]]+)?$ ]]; then
        fail "invalid package '$package' at $PACKAGES_FILE:$line_number"
      fi
      packages+=("$package")
    done
  done < "$PACKAGES_FILE"

  ((${#packages[@]} > 0)) || fail "package list is empty: $PACKAGES_FILE"
fi

if [[ "$SKIP_EXTRA" == false ]]; then
  [[ -r "$EXTRA_SCRIPT" ]] || fail "extra customization script is not readable: $EXTRA_SCRIPT"
  bash -n "$EXTRA_SCRIPT" || fail "extra customization script has invalid syntax"
fi

if [[ "$DRY_RUN" == true ]]; then
  log "Dry run; no files or packages will be changed"
  if [[ "$SKIP_PACKAGES" == false ]]; then
    printf 'Packages (%d) from %s:\n' "${#packages[@]}" "$PACKAGES_FILE"
    print_command apt-get update
    print_command apt-get install --yes "${packages[@]}"
  else
    printf 'Package installation: skipped\n'
  fi

  if [[ "$SKIP_EXTRA" == false ]]; then
    printf 'Extra customization: %s\n' "$EXTRA_SCRIPT"
  else
    printf 'Extra customization: skipped\n'
  fi
  exit 0
fi

((EUID == 0)) || fail "run this script as root inside Cubic's Terminal page (do not use sudo there)"
command -v apt-get >/dev/null 2>&1 || fail "apt-get was not found; an Ubuntu-based Cubic image is required"

if [[ "$ALLOW_HOST" == false ]] && ! is_chroot; then
  fail "no chroot detected; open Cubic's Terminal page and run the script there"
fi

if [[ -r /etc/os-release ]]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  case "${ID:-}" in
    ubuntu|linuxmint|pop) ;;
    *) log "Warning: ${PRETTY_NAME:-this OS} is not a tested Ubuntu desktop base" ;;
  esac
fi

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Cubic's filesystem is not a running OS. Prevent package post-install scripts
# from attempting to start services, while preserving a policy Cubic supplied.
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

log "Applying OmniOS customizations in the Cubic environment"

if [[ "$SKIP_PACKAGES" == false ]]; then
  log "Refreshing Ubuntu package indexes"
  apt-get update

  log "Installing ${#packages[@]} packages from $PACKAGES_FILE"
  apt-get install --yes "${packages[@]}"
else
  log "Package installation skipped"
fi

if [[ "$SKIP_EXTRA" == false ]]; then
  log "Running $EXTRA_SCRIPT"
  OMNIOS_ASSETS_DIR="$REPO_ROOT/assets" bash "$EXTRA_SCRIPT"
else
  log "Extra customization skipped"
fi

if [[ "$KEEP_APT_CACHE" == false ]]; then
  log "Cleaning package caches to reduce the ISO size"
  apt-get clean
  rm -rf /var/lib/apt/lists/*
fi

cleanup
trap - EXIT HUP INT TERM
log "OmniOS customization complete; return to Cubic and click Next"
