#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
required=(
    version.txt
    auto/config
    branding/boot-splash.svg
    scripts/read-version.sh
    scripts/build-vars.sh
    scripts/bump-version.sh
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

bash -n "$ROOT"/scripts/*.sh "$ROOT"/config/hooks/normal/*.hook.* \
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
payload = re.search(r'^PAYLOAD=\((.*?)^\)', package_script, re.S | re.M)
if not payload:
    raise SystemExit('could not read PAYLOAD from scripts/build-package.sh')
includes = root.joinpath('config/includes.chroot')
for line in payload.group(1).splitlines():
    entry = line.split('#', 1)[0].strip()
    if not entry:
        continue
    if not includes.joinpath(entry).exists():
        raise SystemExit(f'omnios-desktop packages a missing path: {entry}')

# The OmniOS apt source must ship disabled. live-build runs "apt update" inside
# the chroot, so an archive that is not published yet answers 404 and fails the
# whole image build. omnios-firstboot enables it once it is reachable.
omnios_sources = root.joinpath(
    'config/includes.chroot/etc/apt/sources.list.d/omnios.sources'
).read_text(encoding='utf-8')
if 'Enabled: no' not in omnios_sources:
    raise SystemExit(
        'omnios.sources must ship with "Enabled: no"; an unpublished archive '
        'returns 404 and fails "apt update" during the image build'
    )

# Diverting a file owned by an Essential package with --rename leaves the
# system without it for a moment, and dpkg warns that this is dangerous.
for candidate in (
    root.joinpath('packaging/omnios-desktop/postinst'),
    root.joinpath('packaging/omnios-desktop/prerm'),
    root.joinpath('config/hooks/normal/9500-omnios-identity.hook.chroot'),
):
    text = candidate.read_text(encoding='utf-8')
    # Join continuation lines so a flag and its target are seen together.
    joined = re.sub(r'\\\s*\n\s*', ' ', text)
    for statement in joined.splitlines():
        if 'dpkg-divert' not in statement or 'os-release' not in statement:
            continue
        if '--rename' in statement.replace('--no-rename', ''):
            raise SystemExit(
                f'{candidate.relative_to(root)}: os-release belongs to '
                'base-files, which is Essential; divert it with --no-rename'
            )

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
    shellcheck "$ROOT"/scripts/*.sh "$ROOT"/config/hooks/normal/*.hook.* \
        "$ROOT"/config/includes.chroot/usr/libexec/omnios-*
fi
# keys/omnios-archive-signing-key.asc is deliberately committed: it signs the
# OmniOS archive, is scoped to this repository, and anyone able to push here
# could replace it anyway. Keeping it in the repository is what lets updates be
# published without any manual secret setup. Every other private key is a
# mistake.
if find "$ROOT" -path "$ROOT/.git" -prune -o -type f \
    \( -name '*.key' -o -name '*.key.pem' \) -print | grep -q .; then
    echo 'ERROR: private key material must not be committed.' >&2
    exit 1
fi

echo 'OmniOS live-build project validation passed.'
