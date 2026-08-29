#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
required=(
    version.txt
    auto/config
    branding/boot-splash.svg
    scripts/read-version.sh
    scripts/build-vars.sh
    scripts/verify-iso.sh
    scripts/build-package.sh
    scripts/build-apt-repo.sh
    packaging/omnios-desktop/control.in
    packaging/omnios-desktop/postinst
    config/includes.chroot/etc/apt/sources.list.d/omnios.sources
    config/package-lists/omnios.list.chroot
    config/hooks/normal/0500-omnios-config.hook.chroot
    config/includes.chroot/usr/share/omnios/identity/os-release
    config/includes.chroot/usr/share/omnios/identity/kcm-about-distrorc
    config/includes.chroot/usr/share/icons/hicolor/scalable/apps/omnios-logo.svg
    config/includes.chroot/usr/share/applications/install-omnios.desktop
    config/includes.chroot/usr/share/omnios/calamares/branding/omnios/branding.desc
    config/includes.chroot/usr/share/wallpapers/OmniOS/contents/images/3840x2160.jpg
)
for path in "${required[@]}"; do
    [[ -s "$ROOT/$path" ]] || { echo "ERROR: required file missing or empty: $path" >&2; exit 1; }
done

bash -n "$ROOT"/scripts/*.sh "$ROOT"/config/hooks/normal/*.hook.chroot \
    "$ROOT"/config/includes.chroot/usr/libexec/omnios-*

python3 - "$ROOT" <<'PY'
from pathlib import Path
import json
import re
import sys
import xml.etree.ElementTree as ET
root = Path(sys.argv[1])
version = root.joinpath('version.txt').read_text(encoding='utf-8').strip()
if not re.fullmatch(r'[0-9]{4}\.[0-9]+(?:\.[0-9]+)?', version):
    raise SystemExit('version.txt must contain one version such as 2026.1')
identity = root.joinpath('config/includes.chroot/usr/share/omnios/identity')
versioned_files = (
    identity.joinpath('issue'),
    identity.joinpath('kcm-about-distrorc'),
    identity.joinpath('lsb-release'),
    identity.joinpath('motd'),
    identity.joinpath('omnios-release'),
    identity.joinpath('os-release'),
    root.joinpath('config/includes.chroot/usr/share/omnios/calamares/branding/omnios/branding.desc'),
)
for path in versioned_files:
    if '@OMNIOS_VERSION@' not in path.read_text(encoding='utf-8'):
        raise SystemExit(f'missing version placeholder: {path.relative_to(root)}')

# The desktop must identify itself as OmniOS, never as the Debian base.
os_release = dict(
    line.split('=', 1)
    for line in identity.joinpath('os-release').read_text(encoding='utf-8').splitlines()
    if '=' in line and not line.startswith('#')
)
for key, expected in (('NAME', '"OmniOS"'), ('ID', 'omnios'), ('LOGO', 'omnios-logo')):
    if os_release.get(key) != expected:
        raise SystemExit(f'os-release {key} must be {expected}, found {os_release.get(key)!r}')
if os_release.get('VERSION_ID') != '"@OMNIOS_VERSION@"':
    raise SystemExit('os-release VERSION_ID must use the OmniOS version placeholder')

# The logo named by os-release has to actually exist, or the desktop falls
# back to the Debian swirl.
logo = os_release['LOGO']
icons = root.joinpath('config/includes.chroot/usr/share/icons/hicolor')
rendered = sorted(icons.glob(f'*/apps/{logo}.png')) + sorted(icons.glob(f'scalable/apps/{logo}.svg'))
if len(rendered) < 2:
    raise SystemExit(f'no icons installed for LOGO={logo}')
for path in rendered:
    if path.suffix == '.png' and path.read_bytes()[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit(f'not a valid PNG: {path.relative_to(root)}')
# Everything the omnios-desktop package claims to ship must exist in the ISO
# tree, otherwise installed systems and the ISO would drift apart.
package_script = root.joinpath('scripts/build-package.sh').read_text(encoding='utf-8')
payload = re.search(r'readonly PAYLOAD=\((.*?)\n\)', package_script, re.S)
if not payload:
    raise SystemExit('could not read PAYLOAD from scripts/build-package.sh')
includes = root.joinpath('config/includes.chroot')
for entry in payload.group(1).split():
    if not includes.joinpath(entry).exists():
        raise SystemExit(f'omnios-desktop packages a missing path: {entry}')

# unattended-upgrades has to accept the OmniOS origin, or published updates
# would never install automatically.
uu = root.joinpath(
    'config/includes.chroot/etc/apt/apt.conf.d/51omnios-unattended-upgrades'
).read_text(encoding='utf-8')
if 'origin=OmniOS' not in uu:
    raise SystemExit('unattended-upgrades does not allow the OmniOS origin')

for path in root.joinpath('config').rglob('*.json'):
    json.loads(path.read_text(encoding='utf-8'))
for path in root.joinpath('branding').rglob('*.svg'):
    ET.parse(path)
packages = []
for line in root.joinpath('config/package-lists/omnios.list.chroot').read_text().splitlines():
    line = line.strip()
    if line and not line.startswith('#'):
        packages.append(line)
if len(packages) != len(set(packages)):
    raise SystemExit('duplicate package names found')
print(f'Validated OmniOS {version} and {len(packages)} explicitly selected packages.')
PY

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$ROOT/config/includes.chroot/usr/share/applications/install-omnios.desktop"
fi
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck "$ROOT"/scripts/*.sh "$ROOT"/config/hooks/normal/*.hook.chroot \
        "$ROOT"/config/includes.chroot/usr/libexec/omnios-*
fi
if find "$ROOT" -path "$ROOT/.git" -prune -o -type f \( -name '*.key' -o -name '*.key.pem' \) -print | grep -q .; then
    echo 'ERROR: private key material must not be committed.' >&2
    exit 1
fi

echo 'OmniOS live-build project validation passed.'
