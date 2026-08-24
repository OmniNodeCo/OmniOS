#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
bash -n "$ROOT"/scripts/*.sh
python3 - <<'PY' "$ROOT"
from pathlib import Path
import json, sys, xml.etree.ElementTree as ET
root=Path(sys.argv[1])/'meta-omnios'
for p in root.rglob('*.json'): json.loads(p.read_text())
for p in root.rglob('*.svg'): ET.parse(p)
print('JSON and SVG assets parsed successfully.')
PY
if command -v kas >/dev/null; then kas dump "$ROOT/kas/omnios-x86_64.yml" >/dev/null; echo 'Kas configuration parsed.'; else echo 'Kas not installed; skipped kas dump.'; fi
if command -v shellcheck >/dev/null; then
 shellcheck "$ROOT"/scripts/*.sh \
  "$ROOT"/meta-omnios/recipes-core/bundles/files/omnios-bundle-hook \
  "$ROOT"/meta-omnios/recipes-core/omnios-persistent-data/files/omnios-persistent-data \
  "$ROOT"/meta-omnios/recipes-core/omnios-update/files/omnios-update
fi
