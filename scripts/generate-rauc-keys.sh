#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
KEY_DIR="$ROOT/.keys/rauc"
command -v openssl >/dev/null || { echo 'ERROR: openssl required' >&2; exit 1; }
mkdir -p "$KEY_DIR"; chmod 0700 "$KEY_DIR"
if [[ -e "$KEY_DIR/ca.key.pem" || -e "$KEY_DIR/ca.cert.pem" ]]; then echo "ERROR: refusing to overwrite $KEY_DIR" >&2; exit 1; fi
openssl req -x509 -newkey rsa:4096 -sha256 -nodes \
 -keyout "$KEY_DIR/ca.key.pem" -out "$KEY_DIR/ca.cert.pem" \
 -days "${OMNIOS_RAUC_KEY_DAYS:-365}" \
 -subj "${OMNIOS_RAUC_KEY_SUBJECT:-/O=OmniOS Development/CN=OmniOS local RAUC signer}"
chmod 0600 "$KEY_DIR/ca.key.pem"; chmod 0644 "$KEY_DIR/ca.cert.pem"
echo "Generated LOCAL DEVELOPMENT keys in $KEY_DIR. Never use this unprotected key for production."
