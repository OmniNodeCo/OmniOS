#!/usr/bin/env python3
"""Render the OmniOS ring logo to PNG icons and a scalable SVG.

The build host is not guaranteed to have a working SVG rasteriser
(librsvg/Inkscape), so the PNGs are rendered here with a small
supersampling rasteriser and written with Python's zlib. This keeps the
icons reproducible from source instead of committing opaque binaries
nobody can regenerate.
"""

from __future__ import annotations

import argparse
import math
import struct
import zlib
from pathlib import Path

# OmniOS brand colours, matching branding/boot-splash.svg and the
# Calamares branding palette.
OUTER_RING = (0x4F, 0xD3, 0xF4)
INNER_RING = (0x1E, 0x6E, 0x8C)

# Geometry expressed as fractions of the icon size so every size matches.
OUTER_RADIUS = 0.460
OUTER_WIDTH = 0.104
INNER_RADIUS = 0.270
INNER_WIDTH = 0.046

SIZES = (16, 22, 24, 32, 48, 64, 128, 256, 512)
SUPERSAMPLE = 4


def _coverage(size: int, radius: float, width: float) -> list[list[float]]:
    """Anti-aliased coverage mask for one ring, via supersampling."""
    centre = size / 2.0
    r_outer = radius * size
    r_inner = r_outer - width * size
    mask = [[0.0] * size for _ in range(size)]
    step = 1.0 / SUPERSAMPLE
    offset = step / 2.0

    for y in range(size):
        row = mask[y]
        for x in range(size):
            hits = 0
            for sy in range(SUPERSAMPLE):
                py = y + offset + sy * step - centre
                for sx in range(SUPERSAMPLE):
                    px = x + offset + sx * step - centre
                    dist = math.hypot(px, py)
                    if r_inner <= dist <= r_outer:
                        hits += 1
            if hits:
                row[x] = hits / (SUPERSAMPLE * SUPERSAMPLE)
    return mask


def _compose(size: int) -> bytearray:
    """Render both rings into a straight-alpha RGBA buffer."""
    outer = _coverage(size, OUTER_RADIUS, OUTER_WIDTH)
    inner = _coverage(size, INNER_RADIUS, INNER_WIDTH)
    buf = bytearray(size * size * 4)

    for y in range(size):
        for x in range(size):
            a_outer = outer[y][x]
            a_inner = inner[y][x]
            alpha = a_outer + a_inner * (1.0 - a_outer)
            idx = (y * size + x) * 4
            if alpha <= 0.0:
                continue
            # Source-over of the inner ring beneath the outer ring.
            for channel in range(3):
                top = OUTER_RING[channel] * a_outer
                bottom = INNER_RING[channel] * a_inner * (1.0 - a_outer)
                buf[idx + channel] = round((top + bottom) / alpha)
            buf[idx + 3] = round(alpha * 255)
    return buf


def _png(size: int, rgba: bytearray) -> bytes:
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)  # filter type 0 (None)
        raw.extend(rgba[y * stride:(y + 1) * stride])

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack('>I', len(payload))
            + tag
            + payload
            + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    header = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return (
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', header)
        + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
        + chunk(b'IEND', b'')
    )


SVG_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <title>OmniOS</title>
  <circle cx="128" cy="128" r="{outer_r:.1f}" fill="none"
          stroke="{outer}" stroke-width="{outer_w:.1f}"/>
  <circle cx="128" cy="128" r="{inner_r:.1f}" fill="none"
          stroke="{inner}" stroke-width="{inner_w:.1f}"/>
</svg>
"""


def _svg() -> str:
    scale = 256
    return SVG_TEMPLATE.format(
        outer_r=(OUTER_RADIUS - OUTER_WIDTH / 2) * scale,
        outer_w=OUTER_WIDTH * scale,
        inner_r=(INNER_RADIUS - INNER_WIDTH / 2) * scale,
        inner_w=INNER_WIDTH * scale,
        outer='#%02X%02X%02X' % OUTER_RING,
        inner='#%02X%02X%02X' % INNER_RING,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        'root',
        nargs='?',
        default=str(Path(__file__).resolve().parent.parent),
        help='repository root (default: the repository containing this script)',
    )
    args = parser.parse_args()
    root = Path(args.root).resolve()

    icons = root / 'config/includes.chroot/usr/share/icons/hicolor'
    branding = root / 'config/includes.chroot/usr/share/omnios/branding'
    branding.mkdir(parents=True, exist_ok=True)

    for size in SIZES:
        target = icons / f'{size}x{size}' / 'apps'
        target.mkdir(parents=True, exist_ok=True)
        data = _png(size, _compose(size))
        (target / 'omnios-logo.png').write_bytes(data)
        if size == 256:
            (branding / 'omnios-logo.png').write_bytes(data)

    scalable = icons / 'scalable' / 'apps'
    scalable.mkdir(parents=True, exist_ok=True)
    svg = _svg()
    (scalable / 'omnios-logo.svg').write_text(svg, encoding='utf-8')
    (branding / 'omnios-logo.svg').write_text(svg, encoding='utf-8')

    print(f'Rendered OmniOS logo icons for sizes: {", ".join(map(str, SIZES))}')


if __name__ == '__main__':
    main()
