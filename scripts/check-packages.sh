#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
PACKAGE_LIST="$ROOT/config/package-lists/omnios.list.chroot"
command -v apt-cache >/dev/null 2>&1 || {
    echo 'ERROR: apt-cache is required for Debian package validation.' >&2
    exit 1
}

missing=()
count=0
while IFS= read -r package; do
    package="${package%%#*}"
    package="${package//[[:space:]]/}"
    [[ -n "$package" ]] || continue
    (( count += 1 ))
    if ! apt-cache show --no-all-versions "$package" >/dev/null 2>&1; then
        missing+=("$package")
    fi
done < "$PACKAGE_LIST"

if (( ${#missing[@]} )); then
    printf 'ERROR: packages unavailable for this Debian release:\n' >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

printf 'Validated availability of %d Debian packages.\n' "$count"
