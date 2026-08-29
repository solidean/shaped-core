#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///
"""Generate the PNG decode fixtures babel's own encoder cannot produce.

babel::png::encode always writes a non-interlaced, non-palette, tRNS-free file at 8 or
16 bits, so every round-trip test exercises exactly one of plan_decode's five branches.
The other four -- palette, tRNS-becomes-alpha, sub-byte grey and Adam7 -- are
decode-only behaviours that png.hh states as fact, and reaching them needs an encoder
we do not have.

So this writes four minimal PNGs by hand, from zlib and struct alone.
Each is a few hundred bytes and targets exactly one branch.

Two outputs, both committed.
The .png files are the fixtures, openable in any viewer, and png_fixtures.hh embeds
their bytes so the test needs no path resolution -- nexus has no test-data mechanism
and this is not the change to add one.

Re-run after touching anything here, then `uv run dev.py format --dirty-only`:

    uv run libs/data/babel-serializer/tests/image/fixtures/make-png-fixtures.py
"""

import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Adam7: (x_start, y_start, x_step, y_step) per pass, in pass order.
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def chunk(kind: bytes, body: bytes) -> bytes:
    """One PNG chunk: length, type, body, CRC over type+body."""
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def ihdr(width: int, height: int, bit_depth: int, color_type: int, interlace: int = 0) -> bytes:
    return chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, interlace))


def png(*chunks: bytes) -> bytes:
    return b"\x89PNG\r\n\x1a\n" + b"".join(chunks) + chunk(b"IEND", b"")


def idat(raw: bytes) -> bytes:
    return chunk(b"IDAT", zlib.compress(raw, 9))


def scanlines(rows: list[bytes]) -> bytes:
    """Filter type 0 (None) in front of every row, which is what the decoder undoes."""
    return b"".join(b"\x00" + row for row in rows)


def indexed_with_trns() -> bytes:
    """4x2 palette PNG whose tRNS makes entry 0 transparent.

    Hits plan_decode's has_trns + non-grey branch: RGBA8, four channels out of a one-byte-per-pixel file.
    """
    palette = bytes([0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255])  # black, red, green, blue
    alpha = bytes([0, 255, 255, 255])  # entry 0 fully transparent, the rest opaque
    rows = [bytes([0, 1, 2, 3]), bytes([3, 2, 1, 0])]
    return png(ihdr(4, 2, 8, 3), chunk(b"PLTE", palette), chunk(b"tRNS", alpha), idat(scanlines(rows)))


def grey_1bit() -> bytes:
    """8x2 one-bit greyscale.

    Hits the sub-byte branch: SPNG_FMT_G8 unpacks the packed bits, so bit_depth is the only place the 1 survives.
    """
    rows = [bytes([0b10101010]), bytes([0b11001100])]
    return png(ihdr(8, 2, 1, 0), idat(scanlines(rows)))


def grey_8bit_with_trns() -> bytes:
    """4x2 greyscale whose tRNS makes one grey level transparent.

    Hits plan_decode's has_trns + grey branch: GA8, so a one-channel file decodes to two.
    """
    rows = [bytes([0, 64, 128, 255]), bytes([255, 128, 64, 0])]
    return png(ihdr(4, 2, 8, 0), chunk(b"tRNS", struct.pack(">H", 128)), idat(scanlines(rows)))


def rgb_adam7() -> bytes:
    """8x8 truecolour, Adam7-interlaced, so every one of the seven passes is non-empty.

    The pixel value is a function of (x, y) alone, which is what lets the test check de-interlacing
    rather than just that a file decoded.
    """

    def pixel(x: int, y: int) -> bytes:
        return bytes([x * 32 % 256, y * 32 % 256, (x + y) * 16 % 256])

    raw = b""
    for x0, y0, dx, dy in ADAM7:
        rows = []
        for y in range(y0, 8, dy):
            rows.append(b"".join(pixel(x, y) for x in range(x0, 8, dx)))
        if rows and rows[0]:
            raw += scanlines(rows)

    return png(ihdr(8, 8, 8, 2, interlace=1), idat(raw))


FIXTURES = {
    "indexed-trns.png": ("png_indexed_trns", indexed_with_trns, "4x2 palette, tRNS on entry 0 -> RGBA8"),
    "grey-1bit.png": ("png_grey_1bit", grey_1bit, "8x2 one-bit grey -> G8, unpacked"),
    "grey-trns.png": ("png_grey_trns", grey_8bit_with_trns, "4x2 8-bit grey, tRNS on 128 -> GA8"),
    "rgb-adam7.png": ("png_rgb_adam7", rgb_adam7, "8x8 rgb, Adam7 across all seven passes"),
}


def as_cpp_array(name: str, data: bytes, what: str) -> str:
    lines = [f"/// {what}"]
    lines.append(f"constexpr unsigned char {name}[] = {{")
    for start in range(0, len(data), 12):
        row = ", ".join(f"0x{b:02X}" for b in data[start : start + 12])
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    blocks = []
    for filename, (symbol, build, what) in FIXTURES.items():
        data = build()
        (HERE / filename).write_bytes(data)
        print(f"{filename}: {len(data)} bytes")
        blocks.append(as_cpp_array(symbol, data, f"{what} ({filename}, {len(data)} bytes)"))

    header = (
        "#pragma once\n\n"
        "// GENERATED by make-png-fixtures.py next to this file — do not edit by hand.\n"
        "// The bytes are the committed .png fixtures beside it, embedded because nexus has no test-data loader.\n"
        "// Each one reaches a plan_decode branch babel::png::encode cannot produce.\n\n"
        "namespace babel_test\n{\n" + "\n\n".join(blocks) + "\n} // namespace babel_test\n"
    )
    (HERE / "png_fixtures.hh").write_text(header, encoding="utf-8", newline="\n")
    print(f"png_fixtures.hh: {len(header)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
