#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
KAS_FILE="${KAS_FILE:-$ROOT/kas/omnios-x86_64.yml}"
KAS_BIN="${KAS_BIN:-$(command -v kas 2>/dev/null || true)}"
[[ -n "$KAS_BIN" ]] || { echo 'ERROR: kas required; run scripts/bootstrap-host.sh' >&2; exit 1; }

prepare_github_actions_userns() {
  [[ "${GITHUB_ACTIONS:-false}" == true ]] || return 0

  local apparmor_userns=/proc/sys/kernel/apparmor_restrict_unprivileged_userns
  if [[ -r "$apparmor_userns" && "$(<"$apparmor_userns")" != 0 ]]; then
    echo 'Enabling unprivileged user namespaces for BitBake on this ephemeral GitHub Actions runner.'
    if (( EUID == 0 )); then
      sysctl -q -w kernel.apparmor_restrict_unprivileged_userns=0
    else
      sudo sysctl -q -w kernel.apparmor_restrict_unprivileged_userns=0
    fi
  fi

  if command -v unshare >/dev/null 2>&1 && ! unshare --user --map-root-user true; then
    echo 'ERROR: user namespaces remain unavailable after GitHub runner preparation.' >&2
    exit 1
  fi
}

ACTION="${1:-build}"
case "$ACTION" in
  build|parse|feed|shell) prepare_github_actions_userns ;;
esac

case "$ACTION" in
 build)
  for f in "$ROOT/.keys/rauc/release.key.pem" "$ROOT/.keys/rauc/release.cert.pem" "$ROOT/.keys/rauc/keyring.pem"; do
   [[ -s "$f" ]] || { echo "ERROR: missing RAUC signing material: $f" >&2; echo 'Run scripts/generate-rauc-keys.sh.' >&2; exit 1; }
  done
  "$KAS_BIN" build "$KAS_FILE" ;;
 checkout) "$KAS_BIN" checkout "$KAS_FILE" ;;
 parse) "$KAS_BIN" shell "$KAS_FILE" -c 'bitbake-layers show-layers && bitbake -p' ;;
 feed) "$KAS_BIN" shell "$KAS_FILE" -c 'bitbake package-index' ;;
 clean) rm -rf -- "$ROOT/build" ;;
 shell) exec "$KAS_BIN" shell "$KAS_FILE" ;;
 *) echo 'Usage: scripts/build.sh [build|checkout|parse|feed|clean|shell]' >&2; exit 2 ;;
esac
