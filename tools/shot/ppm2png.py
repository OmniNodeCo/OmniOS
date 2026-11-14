#!/usr/bin/env python3
"""Convert PPM (P6) screenshots from tools/shot to PNG, with only the
standard library. Optional --scale N shrinks by N (box filter)."""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def shrink(w, h, px, s):
    nw, nh = w // s, h // s
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        for x in range(nw):
            acc = [0, 0, 0]
            for dy in range(s):
                o = ((y * s + dy) * w + x * s) * 3
                for dx in range(s):
                    acc[0] += px[o]
                    acc[1] += px[o + 1]
                    acc[2] += px[o + 2]
                    o += 3
            n = s * s
            k = (y * nw + x) * 3
            out[k] = acc[0] // n
            out[k + 1] = acc[1] // n
            out[k + 2] = acc[2] // n
    return nw, nh, bytes(out)


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    scale = 1
    if args and args[0] == "--scale":
        scale = int(args[1])
        args = args[2:]
    for p in args:
        w, h, px = read_ppm(p)
        if scale > 1:
            w, h, px = shrink(w, h, px, scale)
        out = p[:-4] + ".png" if p.endswith(".ppm") else p + ".png"
        write_png(out, w, h, px)
        print(out)


if __name__ == "__main__":
    main()
