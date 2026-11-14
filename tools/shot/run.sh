#!/bin/sh
# OmniOS — tools/shot/run.sh: build the screenshot tool and the apps, play
# the steps given (see shot.c) and convert the images to PNG.
#
#   tools/shot/run.sh [-o outdir] step...
#   e.g. tools/shot/run.sh unlock shot:desktop.ppm menu shot:start.ppm
#
# CC defaults to musl-gcc from the build tree; any C compiler for Linux
# works (CC="zig cc -target x86_64-linux-musl").
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/build/shots
if [ "${1:-}" = "-o" ]; then
    OUT=$2
    shift 2
fi
CC=${CC:-$ROOT/build/sysroot/usr/bin/musl-gcc}
mkdir -p "$OUT"
make -s -C "$ROOT/os" MUSL_GCC="$CC" all
# shellcheck disable=SC2086
$CC -std=c99 -Os -D_GNU_SOURCE -I "$ROOT/os/lib" -I "$ROOT/os/gui" \
    -I "$ROOT/os/kernel" -o "$OUT/shot" "$ROOT/tools/shot/shot.c" \
    "$ROOT"/os/gui/wm.c "$ROOT"/os/gui/protocol.c "$ROOT"/os/gui/catalog.c \
    "$ROOT"/os/gui/theme.c "$ROOT"/os/gui/icons.c "$ROOT"/os/gui/font.c "$ROOT"/os/gui/scenes.c \
    "$ROOT"/os/gui/settings.c \
    "$ROOT"/os/gui/account.c "$ROOT"/os/gui/updstat.c "$ROOT"/os/gui/netinfo.c \
    "$ROOT"/os/lib/raster.c "$ROOT"/os/lib/canvas.c "$ROOT"/os/lib/osfb.c \
    "$ROOT"/os/lib/input.c "$ROOT"/os/lib/devinput.c "$ROOT"/os/lib/gfx.c \
    -static -lm
cd "$OUT"
OMNI_FONT_DIR=${OMNI_FONT_DIR:-$ROOT/os/vendor/fonts/selawik} \
OMNI_SETTINGS=${OMNI_SETTINGS:-$OUT/settings} \
OMNI_APPS_STATE=${OMNI_APPS_STATE:-$OUT/installed-apps} \
    ./shot -b "$ROOT/build/install/os/bin" "$@"
for f in *.ppm; do
    [ -e "$f" ] || continue
    python3 "$ROOT/tools/shot/ppm2png.py" "$f" >/dev/null && rm -f "$f"
done
ls "$OUT"/*.png 2>/dev/null || true
