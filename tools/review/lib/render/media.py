"""What a file IS, before anything decides how to show it.

Every viewer here used to assume text.
A reference to a committed JPEG therefore went through `read_text(errors="replace")` and into the highlighter, which
produced a screen of replacement characters, slowly, and told the reader nothing about the picture they pointed at.
A review that carries reference images — and one that reviews the branch adding them certainly does — meets that on
its first hover.

So classification happens once, here, and the popover and the file page both branch on the answer.

The rule is deliberately dumb.
An extension we know how to draw makes a file an image; a NUL in the first block makes it binary; everything else is
text and is highlighted as before.
Sniffing content to second-guess an extension would buy accuracy nobody needs and a way to be wrong about a file the
repository is perfectly clear about.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

# Read far enough to be sure, and no further: a NUL in the first block is what every tool uses to call a file binary,
# and a file with none in 8 kB does not hide one in the middle often enough to pay for reading it all.
SNIFF_BYTES = 8192

# The extensions worth drawing rather than describing, and what to serve them as.
#
# SVG is text and is still here: a reader hovering a diagram wants the diagram.
# It is served into an `<img>`, which does not run script, so being text does not make it a different kind of risk
# from the raster formats beside it.
IMAGE_TYPES = {
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".webp": "image/webp",
    ".bmp": "image/bmp",
    ".ico": "image/x-icon",
    ".avif": "image/avif",
    ".svg": "image/svg+xml",
}

TEXT = "text"
IMAGE = "image"
BINARY = "binary"


@dataclass(frozen=True)
class FileKind:
    """How one file should be shown, and what a head line can say about it."""

    kind: str = TEXT
    mime: str = ""
    size: int = 0

    # Pixels, where the format's header gave them up cheaply; 0 when it did not.
    # A head line saying `1280x720` is the whole of what a reader learns about an image before it paints, so it is
    # worth the twenty lines below — but never worth decoding the file to find out.
    width: int = 0
    height: int = 0

    @property
    def is_text(self) -> bool:
        return self.kind == TEXT

    @property
    def dimensions(self) -> str:
        return f"{self.width}×{self.height}" if self.width and self.height else ""


def classify(target: Path) -> FileKind:
    """What `target` is, from its extension and its first block.

    An unreadable file classifies as text, so the caller's existing read-and-report path produces the error message
    rather than this one inventing a second one.
    """
    try:
        size = target.stat().st_size
        head = target.open("rb").read(SNIFF_BYTES)
    except OSError:
        return FileKind()

    mime = IMAGE_TYPES.get(target.suffix.lower(), "")
    if mime:
        width, height = image_dimensions(head)
        return FileKind(kind=IMAGE, mime=mime, size=size, width=width, height=height)

    if b"\0" in head:
        return FileKind(kind=BINARY, mime="application/octet-stream", size=size)

    return FileKind(kind=TEXT, size=size)


def image_dimensions(head: bytes) -> tuple[int, int]:
    """`(width, height)` for the raster formats whose header states it, `(0, 0)` for the rest.

    PNG and GIF put it at a fixed offset.
    JPEG does not: the size lives in whichever SOF segment appears first, so the marker chain has to be walked.
    WEBP, AVIF, BMP and ICO are not here — nothing in this repository references one yet, and a wrong number is worse
    than no number.
    """
    if head.startswith(b"\x89PNG\r\n\x1a\n") and len(head) >= 24:
        width, height = struct.unpack(">II", head[16:24])
        return int(width), int(height)

    if head[:6] in (b"GIF87a", b"GIF89a") and len(head) >= 10:
        width, height = struct.unpack("<HH", head[6:10])
        return int(width), int(height)

    if head.startswith(b"\xff\xd8"):
        return _jpeg_dimensions(head)

    return 0, 0


def _jpeg_dimensions(head: bytes) -> tuple[int, int]:
    """Walk the marker chain to the first start-of-frame, which is where a JPEG states its size.

    Bounded by what was sniffed rather than by the file: a JPEG whose SOF sits past the first block reports nothing,
    which is the honest answer for a header this did not read.
    """
    i = 2
    while i + 9 < len(head):
        if head[i] != 0xFF:
            i += 1 # a fill byte, or a chain this cannot follow; step rather than give up
            continue

        marker = head[i + 1]
        # SOF0..SOF15, minus the three that are not frame headers at all.
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            height, width = struct.unpack(">HH", head[i + 5:i + 9])
            return int(width), int(height)

        # Standalone markers carry no length field, so there is nothing to skip over.
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue

        length = int(struct.unpack(">H", head[i + 2:i + 4])[0])
        if length < 2:
            return 0, 0
        i += 2 + length

    return 0, 0


def human_bytes(size: int) -> str:
    for unit, cut in (("MB", 1024 * 1024), ("kB", 1024)):
        if size >= cut:
            return f"{size / cut:.1f} {unit}"
    return f"{size} B"
