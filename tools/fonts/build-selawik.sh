#!/bin/sh
# OmniOS — tools/fonts/build-selawik.sh
#
# Rebuild os/vendor/fonts/selawik/*.ttf from Microsoft's Selawik sources
# (SIL Open Font License 1.1). Selawik is an open-source font with the
# same metrics as Segoe UI, the Windows interface font; OmniOS's desktop
# and apps draw their text with it.
#
#   tools/fonts/build-selawik.sh            (needs git and python3)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REV=89362e84731d1f5777fa078fd4d5ebbd339e4378        # Selawik, 2023-06-05
WORK=${TMPDIR:-/tmp}/omnios-selawik
OUT=$ROOT/os/vendor/fonts/selawik

rm -rf "$WORK"
git clone -q https://github.com/microsoft/Selawik.git "$WORK/src"
git -C "$WORK/src" checkout -q "$REV"
python3 -m venv "$WORK/venv"
"$WORK/venv/bin/pip" install -q fontmake
for w in Regular Bold Light; do
    "$WORK/venv/bin/fontmake" -u "$WORK/src/Source files/UFO/Selawik-$w.ufo" \
        -o ttf --output-dir "$OUT"
done
cp "$WORK/src/LICENSE.txt" "$OUT/OFL.txt"
ls -l "$OUT"
