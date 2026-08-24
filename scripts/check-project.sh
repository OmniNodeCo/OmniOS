#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
required=(
    auto/config
    branding/boot-splash.svg
    config/package-lists/omnios.list.chroot
    config/hooks/normal/0500-omnios-config.hook.chroot
    config/includes.chroot/usr/share/omnios/identity/os-release
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
import sys
import xml.etree.ElementTree as ET
root = Path(sys.argv[1])
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
print(f'Validated {len(packages)} explicitly selected packages.')
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
