#!/usr/bin/env bash
set -Eeuo pipefail
[[ "$(uname -s)" == Linux ]] || { echo 'ERROR: Linux host required' >&2; exit 1; }
command -v apt-get >/dev/null || { echo 'ERROR: automatic bootstrap supports apt hosts only' >&2; exit 1; }
sudo apt-get update
sudo apt-get install --yes \
 bmap-tools build-essential chrpath cpio debianutils diffstat file gawk gcc git \
 iputils-ping libacl1 liblz4-tool locales openssl ovmf python3 python3-git \
 python3-jinja2 python3-pexpect python3-pip python3-subunit python3-venv \
 qemu-system-x86 qemu-utils socat texinfo unzip wget xz-utils zstd
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
python3 -m venv "$ROOT/.venv"
"$ROOT/.venv/bin/pip" install --upgrade 'kas==5.5' shellcheck-py
cat <<'TXT'
Host setup complete:
  source .venv/bin/activate
  scripts/generate-rauc-keys.sh
  scripts/build.sh build
TXT
