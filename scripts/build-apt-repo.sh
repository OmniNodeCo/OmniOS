#!/usr/bin/env bash
# Build a signed OmniOS APT repository around the omnios-desktop package.
#
# The result is a plain directory tree that can be served by any static host,
# including GitHub Pages. Installed systems point at it and therefore receive
# OmniOS updates through the same unattended-upgrades path that already
# delivers Debian security updates.
#
# Signing:
#   OMNIOS_GPG_KEY   key id or fingerprint to sign with (optional)
#   OMNIOS_REPO_URL  public URL of the repo, recorded in the sources file
#
# Without a key the repository is still built but left unsigned, which APT will
# refuse by default. That is deliberate: an unsigned repository is only useful
# for local testing.
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION="$("$ROOT/scripts/read-version.sh")"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
REPO_DIR="${REPO_DIR:-$OUT_DIR/repo}"
SUITE="${OMNIOS_SUITE:-stable}"
COMPONENT="main"
ARCH="amd64"
REPO_URL="${OMNIOS_REPO_URL:-https://omninodeco.github.io/OmniOS/repo}"

command -v dpkg-scanpackages >/dev/null 2>&1 || {
    echo 'ERROR: dpkg-dev is required (provides dpkg-scanpackages).' >&2
    exit 1
}

deb="$OUT_DIR/omnios-desktop_${VERSION}_all.deb"
[[ -f "$deb" ]] || {
    echo "ERROR: $deb not found. Run scripts/build-package.sh first." >&2
    exit 1
}

pool="$REPO_DIR/pool/$COMPONENT"
dist="$REPO_DIR/dists/$SUITE"
binary="$dist/$COMPONENT/binary-$ARCH"
rm -rf "$REPO_DIR"
install -d "$pool" "$binary"
cp "$deb" "$pool/"

( cd "$REPO_DIR" && dpkg-scanpackages --arch "$ARCH" pool > "$binary/Packages" )
gzip -9 -c "$binary/Packages" > "$binary/Packages.gz"

cat > "$binary/Release" <<EOF
Archive: $SUITE
Component: $COMPONENT
Origin: OmniOS
Label: OmniOS
Architecture: $ARCH
EOF

# The top level Release file is what APT verifies, so the checksums of every
# index have to be listed in it.
{
    echo "Origin: OmniOS"
    echo "Label: OmniOS"
    echo "Suite: $SUITE"
    echo "Codename: $SUITE"
    echo "Version: $VERSION"
    echo "Architectures: $ARCH"
    echo "Components: $COMPONENT"
    echo "Description: OmniOS Desktop updates"
    echo "Date: $(date -Ru -d "@${SOURCE_DATE_EPOCH:-$(date +%s)}" | sed 's/+0000/UTC/')"
} > "$dist/Release"

hash_index() {
    local field="$1" prog="$2"
    echo "$field:" >> "$dist/Release"
    while IFS= read -r -d '' file; do
        local rel size sum
        rel="${file#"$dist"/}"
        size="$(stat -c '%s' "$file")"
        sum="$("$prog" "$file" | cut -d' ' -f1)"
        printf ' %s %16d %s\n' "$sum" "$size" "$rel" >> "$dist/Release"
    done < <(find "$dist" -type f ! -name Release ! -name 'Release.gpg' \
        ! -name InRelease -print0 | sort -z)
}
hash_index MD5Sum md5sum
hash_index SHA256 sha256sum

if [[ -n "${OMNIOS_GPG_KEY:-}" ]]; then
    gpg --batch --yes --default-key "$OMNIOS_GPG_KEY" \
        --clearsign --output "$dist/InRelease" "$dist/Release"
    gpg --batch --yes --default-key "$OMNIOS_GPG_KEY" \
        --armor --detach-sign --output "$dist/Release.gpg" "$dist/Release"
    gpg --batch --yes --export --armor "$OMNIOS_GPG_KEY" \
        > "$REPO_DIR/omnios-archive-keyring.asc"
    echo "Signed the repository with $OMNIOS_GPG_KEY"
else
    echo '::warning::OMNIOS_GPG_KEY is not set; the repository is UNSIGNED and APT will reject it.' >&2
fi

# The sources file installed systems use, in deb822 format.
cat > "$REPO_DIR/omnios.sources" <<EOF
Types: deb
URIs: $REPO_URL
Suites: $SUITE
Components: $COMPONENT
Architectures: $ARCH
Signed-By: /usr/share/keyrings/omnios-archive-keyring.asc
EOF

printf 'Built APT repository at %s\n' "$REPO_DIR"
find "$REPO_DIR" -type f -printf '  %P\n' | sort
