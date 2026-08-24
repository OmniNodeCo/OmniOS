#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
KEY_DIR="$ROOT/.keys/rauc"
command -v openssl >/dev/null || { echo 'ERROR: openssl required' >&2; exit 1; }
mkdir -p "$KEY_DIR"
chmod 0700 "$KEY_DIR"
for path in release.key.pem release.cert.pem keyring.pem; do
  if [[ -e "$KEY_DIR/$path" ]]; then
    echo "ERROR: refusing to overwrite $KEY_DIR" >&2
    exit 1
  fi
done
openssl req -x509 -newkey rsa:4096 -sha256 -nodes \
  -keyout "$KEY_DIR/release.key.pem" \
  -out "$KEY_DIR/release.cert.pem" \
  -days "${OMNIOS_RAUC_KEY_DAYS:-365}" \
  -subj "${OMNIOS_RAUC_KEY_SUBJECT:-/O=OmniOS Development/CN=OmniOS local RAUC signer}"
cp "$KEY_DIR/release.cert.pem" "$KEY_DIR/keyring.pem"
chmod 0600 "$KEY_DIR/release.key.pem"
chmod 0644 "$KEY_DIR/release.cert.pem" "$KEY_DIR/keyring.pem"
echo "Generated LOCAL DEVELOPMENT keys in $KEY_DIR. Never use this unprotected key for production."
