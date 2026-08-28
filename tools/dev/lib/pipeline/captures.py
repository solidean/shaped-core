"""Capture sidecars: which examples can be captured, and how.

An example is capturable when the directory it lives in carries a `.capture.json` naming it.
**An example with no sidecar entry is never launched by a capture sweep**, which is what makes "the sweep opens no
window" true by construction rather than by detection — and that is the whole reason a sweep is allowed to exist
against the deliberately refused `--all`.

It is also where per-example knowledge goes that does not belong in the source.
How many frames a scene needs to converge is a property of the content, not of the API being demonstrated, so putting
it here keeps the example itself at zero added lines while still letting a heavy scene ask for what it needs.

Discovery used to be a runtime probe — launch the binary, run one frame, parse the names it printed.
That cost a process launch per example, needed a print prefix to survive the test runner writing to the same stream,
and could not say anything at all about an example that does not use `sv::interactive`.

JSON rather than YAML because `dev.py` declares `dependencies = []` and is worth keeping that way; see docs/dev-py-driver.md.
Full-line `//` comments are stripped before parsing, so the file still reads like configuration.

Public API:
    load_sidecar(directory) -> dict[str, ExampleCapture]
    captures_for(example, root) -> list[Capture] | None
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

SIDECAR_NAME = ".capture.json"

# What a sidecar may say when it says nothing.
_DEFAULT_SIZE = "1920x1080"
_DEFAULT_MECHANISM = "sv"

# The mechanisms that exist, and what each one is expected to PRODUCE.
#
# `sv` is a run driven through sv::interactive: it reads the SC_CAPTURE_* variables and needs no code in the example.
# `custom` is a program that answers the same variables itself, which is what an app owning its own loop must do —
# `examples/vdoc/cube-editor` is the worked example.
# `transcript` is a text example, whose artifact is its stdout.
#
# Declaring this is what removes the guessing.
# Deciding after the fact — "an image appeared, so it was graphical" — reads a failed image capture as a successful text
# one, which is how a broken capture becomes a committed transcript.
MECHANISMS = ("sv", "custom", "transcript")

# The container formats an image capture may ask for.
#
# Checked here rather than trusted, because the extension is the WHOLE rule downstream: `sr::write_capture_image`
# picks the encoder from it and falls back to JPEG, and the refresh step copies only a known image suffix.
# So an unknown format writes JPEG bytes under a name nothing recognises, and the refresh silently skips it.
FORMATS = ("jpg", "jpeg", "png")

# The mechanisms whose artifact is an image file at SC_CAPTURE_OUT.
#
# `sv` and `custom` differ only in who implements the protocol, never in the contract: an image must appear at the path the run was given.
# That is why both are here, and why dev.py needs no branch on which one it got.
IMAGE_MECHANISMS = ("sv", "custom")


@dataclass(frozen=True)
class Capture:
    """One image to take from one example."""

    example: str
    name: str  # the `register_capture` name; empty is the view the example's own body leaves
    mechanism: str
    size: str
    fmt: str
    accumulate: int | None
    timeout: float | None

    @property
    def slug(self) -> str:
        """What this capture's folder and committed file are named after."""
        return self.name or "default"


@dataclass
class ExampleCapture:
    """One example's entry in a sidecar."""

    mechanism: str = _DEFAULT_MECHANISM
    size: str = _DEFAULT_SIZE
    fmt: str = "jpg"
    accumulate: int | None = None
    timeout: float | None = None
    captures: list[dict] = field(default_factory=lambda: [{}])


class SidecarError(Exception):
    """A sidecar that cannot be trusted to say what it means.

    Raised rather than defaulted: a misspelled key here would silently capture the wrong thing, and a reference image
    is exactly the kind of artifact nobody re-checks once it looks plausible.
    """


def _strip_comments(text: str) -> str:
    """Drop whole-line `//` comments, so a sidecar can be commented without leaving JSON.

    Whole-line only, deliberately: a `//` inside a string is a path, and stripping those would corrupt the value
    rather than the file.
    """
    return "\n".join("" if line.lstrip().startswith("//") else line for line in text.splitlines())


def load_sidecar(directory: Path) -> dict[str, ExampleCapture]:
    """The sidecar in `directory`, or an empty mapping when it has none."""
    path = directory / SIDECAR_NAME
    if not path.is_file():
        return {}

    try:
        raw = json.loads(_strip_comments(path.read_text(encoding="utf-8")) or "{}")
    except ValueError as e:
        raise SidecarError(f"{path}: {e}") from e

    if not isinstance(raw, dict):
        raise SidecarError(f"{path}: the top level must be an object keyed by example name")

    out: dict[str, ExampleCapture] = {}
    for name, body in raw.items():
        if not isinstance(body, dict):
            raise SidecarError(f"{path}: {name!r} must map to an object")

        unknown = set(body) - {"mechanism", "size", "format", "accumulate", "timeout", "captures"}
        if unknown:
            raise SidecarError(f"{path}: {name!r} has unknown key(s): {', '.join(sorted(unknown))}")

        entry = ExampleCapture(
            mechanism=body.get("mechanism", _DEFAULT_MECHANISM),
            size=body.get("size", _DEFAULT_SIZE),
            fmt=body.get("format", "jpg"),
            accumulate=body.get("accumulate"),
            timeout=body.get("timeout"),
            captures=body.get("captures", [{}]),
        )
        if entry.mechanism not in MECHANISMS:
            raise SidecarError(f"{path}: {name!r} asks for mechanism {entry.mechanism!r}; known: {', '.join(MECHANISMS)}")
        if entry.fmt not in FORMATS:
            raise SidecarError(f"{path}: {name!r} asks for format {entry.fmt!r}; known: {', '.join(FORMATS)}")
        if not isinstance(entry.captures, list) or not entry.captures:
            raise SidecarError(f"{path}: {name!r} must list at least one capture")

        out[name] = entry

    return out


def captures_for(example, root: Path) -> list[Capture] | None:
    """Every capture `example` declares, or None when it declares no sidecar entry at all.

    None and an empty list are different answers: None means "this example is not capturable", which is what the sweep
    skips on, and it is why an example that renders through something other than `sv::interactive` is never launched.
    """
    directory = Path(example.file).parent
    if not directory.is_dir():
        return None

    entry = load_sidecar(directory).get(example.name)
    if entry is None:
        return None

    out: list[Capture] = []
    for spec in entry.captures:
        if not isinstance(spec, dict):
            raise SidecarError(f"{directory / SIDECAR_NAME}: each capture of {example.name!r} must be an object")

        unknown = set(spec) - {"name", "size", "format", "accumulate", "timeout"}
        if unknown:
            raise SidecarError(
                f"{directory / SIDECAR_NAME}: a capture of {example.name!r} has unknown key(s): {', '.join(sorted(unknown))}"
            )

        fmt = spec.get("format", entry.fmt)
        if fmt not in FORMATS:
            raise SidecarError(
                f"{directory / SIDECAR_NAME}: a capture of {example.name!r} asks for format {fmt!r}; "
                f"known: {', '.join(FORMATS)}"
            )

        out.append(Capture(
            example=example.name,
            name=spec.get("name", ""),
            mechanism=entry.mechanism,
            size=spec.get("size", entry.size),
            fmt=fmt,
            accumulate=spec.get("accumulate", entry.accumulate),
            timeout=spec.get("timeout", entry.timeout),
        ))
    return out
