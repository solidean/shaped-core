"""What an SGL package's entries mean, read from the compiler rather than parsed here.

An HLSL package's typed entries are read by binding_grammar.py, a second parser of that language.
SGL has exactly one parser, the compiler, so this asks it: `sgl describe <file>` prints what a host side is
generated from, and everything below is that JSON and nothing SGL-shaped of its own.

The JSON is the compiler's private answer to this generator; driver/describe.hh is what it says.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

# An SGL package spells its stages as SGL does, and `pixel` is sg's fragment stage.
# The word is the package's and the generated symbol's; sg has one stage, so the enumerator stays `fragment`.
SGL_STAGES = {"vertex": "vertex", "pixel": "fragment", "compute": "compute"}

# The kinds that generate C++ from a declaration rather than naming an entry point, and what `describe` lists each under.
BINDING_KIND = "binding"
VERTEX_INPUT_KIND = "vertex_input"
RENDER_TARGET_KIND = "render_target"
TYPED_KINDS = (BINDING_KIND, VERTEX_INPUT_KIND, RENDER_TARGET_KIND)

# `path:*` is every entry point and every typed declaration the file holds.
EVERYTHING = "*"


class DescriptionError(Exception):
    """The compiler refused a file, or a declaration names something the file does not hold."""


@dataclass
class SglFile:
    """One SGL source, as the compiler described it."""

    path: str
    bindings: list[dict] = field(default_factory=list)
    structs: list[dict] = field(default_factory=list)
    entry_points: list[dict] = field(default_factory=list)

    def binding(self, name: str) -> dict | None:
        return next((b for b in self.bindings if b["name"] == name), None)

    def struct(self, name: str, edge: str) -> dict | None:
        return next((s for s in self.structs if s["name"] == name and s["edge"] == edge), None)

    def entry_point(self, name: str) -> dict | None:
        return next((e for e in self.entry_points if e["name"] == name), None)


@dataclass
class SglEntries:
    """What an SGL package asked for, after `*` is expanded and every name is checked against its file."""

    # (path, stage, entry point) in the package's stage words; the stage is the one the source declares.
    entry_points: list[tuple[str, str, str]] = field(default_factory=list)
    # (file, the described binding)
    bindings: list[tuple[SglFile, dict]] = field(default_factory=list)
    vertex_inputs: list[tuple[SglFile, dict]] = field(default_factory=list)
    render_targets: list[tuple[SglFile, dict]] = field(default_factory=list)


def describe(tool: Path, source: Path, shown_as: str) -> SglFile:
    """Runs the compiler over one file; its diagnostics become the error, word for word."""
    result = subprocess.run([str(tool), "describe", str(source)], capture_output=True, encoding="utf-8")
    if result.returncode != 0:
        said = (result.stderr or result.stdout).strip()
        raise DescriptionError(f"'{shown_as}' does not compile, so nothing is generated from it:\n{said}")
    data = json.loads(result.stdout)
    return SglFile(path=shown_as, bindings=data["bindings"], structs=data["structs"],
                   entry_points=data["entry_points"])


def resolve(package: str, entries: list[str], source_dir: Path, tool: Path | None) -> SglEntries:
    """Every entry of an SGL package, with `*` expanded and every declared name found in its file.

    An entry naming an entry point needs no compiler, so a package of those alone works where no `sgl` can run.
    """
    out = SglEntries()
    files: dict[str, SglFile] = {}
    seen: set[tuple] = set()

    def file_of(path: str) -> SglFile:
        if path not in files:
            if tool is None:
                raise DescriptionError(
                    f"shader package '{package}': '{path}' needs the SGL compiler to generate its C++, and this "
                    f"build has none; see SC_SGL_TOOL in ShaderPackage.cmake")
            try:
                files[path] = describe(tool, source_dir / path, path)
            except DescriptionError as e:
                raise DescriptionError(f"shader package '{package}': {e}") from e
        return files[path]

    def add(kind: str, key: tuple, item) -> None:
        # `path:*` beside an explicit entry for the same thing asks for it once.
        if (kind, *key) in seen:
            return
        seen.add((kind, *key))
        getattr(out, kind).append(item)

    for entry in entries:
        parts = entry.split(":")
        path = parts[0]

        if parts[1:] == [EVERYTHING]:
            described = file_of(path)
            for e in described.entry_points:
                add("entry_points", (path, e["name"]), (path, e["stage"], e["name"]))
            for b in described.bindings:
                add("bindings", (path, b["name"]), (described, b))
            for s in described.structs:
                kind = "vertex_inputs" if s["edge"] == "vertex" else "render_targets"
                add(kind, (path, s["name"]), (described, s))
            continue

        if len(parts) != 3:
            raise DescriptionError(
                f"shader package '{package}': entry '{entry}' must be path:*, path:stage:entry_point, "
                f"path:{BINDING_KIND}:name, path:{VERTEX_INPUT_KIND}:struct or path:{RENDER_TARGET_KIND}:struct")
        _, kind, name = parts

        if kind in SGL_STAGES:
            add("entry_points", (path, name), (path, kind, name))
            continue

        if kind not in TYPED_KINDS:
            raise DescriptionError(
                f"shader package '{package}': entry '{entry}' has kind '{kind}'. An SGL package spells its stages as "
                f"SGL does ({' '.join(SGL_STAGES)}), and generates from {' '.join(TYPED_KINDS)}")

        described = file_of(path)
        if kind == BINDING_KIND:
            found = described.binding(name)
            listed = [b["name"] for b in described.bindings]
            target = "bindings"
        else:
            edge = "vertex" if kind == VERTEX_INPUT_KIND else "pixel"
            found = described.struct(name, edge)
            listed = [s["name"] for s in described.structs if s["edge"] == edge]
            target = "vertex_inputs" if kind == VERTEX_INPUT_KIND else "render_targets"
        if found is None:
            held = ", ".join(listed) or "none"
            raise DescriptionError(
                f"shader package '{package}': entry '{entry}' names '{name}', which '{path}' does not declare "
                f"(it holds: {held})")
        add(target, (path, name), (described, found))

    return out
