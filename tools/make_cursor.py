#!/usr/bin/env python3
"""Generate rootfs/cursor.bmp — the winman pointer sprite.

Emits a 32-bit BITMAPV4HEADER BMP with an explicit alpha mask, which is
what an image editor produces when it exports RGBA. Run it to regenerate
the default arrow; to use a hand-drawn cursor instead, just overwrite
rootfs/cursor.bmp with any 24- or 32-bit uncompressed BMP and rebuild.

    python tools/make_cursor.py

The mask below is the same shape winman carries as its built-in fallback
(bin/winman/winman.c), so a fresh build looks identical whether or not
the file is present. 0 = transparent, 1 = black border, 2 = white fill.
"""
import os
import struct

MASK = [
    [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0],
    [1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0],
    [1, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0],
    [1, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0],
    [0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0],
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
]

# BGRA, little-endian, premultiplied by nothing — straight alpha.
PALETTE = {
    0: b"\x00\x00\x00\x00",   # transparent
    1: b"\x00\x00\x00\xFF",   # opaque black
    2: b"\xFF\xFF\xFF\xFF",   # opaque white
}

DIB_SIZE = 108          # BITMAPV4HEADER
FILE_HEADER = 14
DATA_OFF = FILE_HEADER + DIB_SIZE


def build() -> bytes:
    height = len(MASK)
    width = len(MASK[0])

    # BMP rows run bottom-up when the height is positive.
    rows = b"".join(
        b"".join(PALETTE[v] for v in MASK[y])
        for y in reversed(range(height))
    )

    dib = struct.pack(
        "<IiiHHIIiiII",
        DIB_SIZE, width, height, 1, 32,
        3,                      # BI_BITFIELDS
        len(rows),
        2835, 2835,             # ~72 DPI
        0, 0,
    )
    dib += struct.pack("<IIII",
                       0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    dib += struct.pack("<I", 0x73524742)      # 'BGRs' — sRGB
    dib += b"\x00" * 36                        # CIE endpoints, unused
    dib += struct.pack("<III", 0, 0, 0)        # gamma, unused

    assert len(dib) == DIB_SIZE, len(dib)

    header = struct.pack("<2sIHHI", b"BM", DATA_OFF + len(rows),
                         0, 0, DATA_OFF)
    return header + dib + rows


def main() -> None:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "rootfs", "cursor.bmp")
    data = build()
    with open(out, "wb") as fp:
        fp.write(data)
    print(f"{out}: {len(data)} bytes, {len(MASK[0])}x{len(MASK)}")


if __name__ == "__main__":
    main()
