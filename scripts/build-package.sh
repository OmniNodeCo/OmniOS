#!/usr/bin/env bash
# Build the omnios-desktop Debian package.
#
# The package carries the OmniOS files that are not Debian packages, taken from
# exactly the same config/includes.chroot tree the ISO is built from. That is
# what lets an installed system receive OmniOS changes through ordinary
# automatic updates instead of only through a new ISO.
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION="$("$ROOT/scripts/read-version.sh")"
OUT_DIR="${OUT_DIR:-$ROOT/out}"
PKG="omnios-desktop"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

command -v dpkg-deb >/dev/null 2>&1 || {
    echo 'ERROR: dpkg-deb is required to build the OmniOS package.' >&2
    exit 1
}

# Everything OmniOS owns, and nothing Debian owns. Debian's own packages are
# upgraded by APT, so shipping copies of their files here would fight dpkg.
readonly PAYLOAD=(
    usr/libexec/omnios-antivirus-scan
    usr/libexec/omnios-apt-snapshot
    usr/libexec/omnios-enable-flathub
    usr/libexec/omnios-firstboot
    usr/libexec/omnios-security-center
    usr/lib/systemd/system/omnios-antivirus-scan.service
    usr/lib/systemd/system/omnios-antivirus-scan.timer
    usr/lib/systemd/system/omnios-firstboot.service
    usr/lib/systemd/system/omnios-flathub.service
    usr/share/applications/omnios-security-center.desktop
    usr/share/omnios
    usr/share/icons/hicolor
    usr/share/wallpapers/OmniOS
    usr/share/plasma/look-and-feel/org.omnios.desktop
    etc/apt/apt.conf.d/51omnios-unattended-upgrades
    etc/apt/apt.conf.d/80omnios-snapshots
    etc/sysctl.d/99-omnios-security.conf
)

src="$ROOT/config/includes.chroot"
build="$STAGE/$PKG"
install -d "$build/DEBIAN"

for path in "${PAYLOAD[@]}"; do
    if [[ ! -e "$src/$path" ]]; then
        echo "ERROR: packaged path is missing from the ISO tree: $path" >&2
        exit 1
    fi
    install -d "$build/$(dirname "$path")"
    cp -a "$src/$path" "$build/$(dirname "$path")/"
done

# Render the version placeholder exactly as scripts/build.sh does for the ISO.
while IFS= read -r -d '' file; do
    sed -i "s/@OMNIOS_VERSION@/$VERSION/g" "$file"
done < <(grep -rlZ '@OMNIOS_VERSION@' "$build" || true)
if grep -R -q '@OMNIOS_VERSION@' "$build"; then
    echo 'ERROR: an OmniOS version placeholder was not rendered.' >&2
    exit 1
fi

sed "s/@OMNIOS_VERSION@/$VERSION/g" \
    "$ROOT/packaging/$PKG/control.in" > "$build/DEBIAN/control"

for script in postinst prerm; do
    if [[ -f "$ROOT/packaging/$PKG/$script" ]]; then
        # dpkg rejects the debhelper token, which is only meaningful to dh.
        grep -v '^#DEBHELPER#$' "$ROOT/packaging/$PKG/$script" \
            > "$build/DEBIAN/$script"
        chmod 0755 "$build/DEBIAN/$script"
    fi
done

# Configuration under /etc must be registered as conffiles so that local edits
# by the user survive an upgrade instead of being silently overwritten.
( cd "$build" && find etc -type f -printf '/%p\n' | sort ) > "$build/DEBIAN/conffiles"

find "$build/usr/libexec" -type f -exec chmod 0755 {} +
find "$build/usr/share" -type f -exec chmod 0644 {} +
find "$build/etc" -type f -exec chmod 0644 {} +

install -d "$OUT_DIR"
deb="$OUT_DIR/${PKG}_${VERSION}_all.deb"
dpkg-deb --root-owner-group --build "$build" "$deb" >/dev/null

printf 'Built %s\n' "$deb"
dpkg-deb --info "$deb" | sed -n '1,12p'
