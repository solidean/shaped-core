"""The `abspaths` gate: no absolute source or build path in a compile definition or a generated source.

A binary that carries such a path depends on where its checkout sits.
No compile is shared between two checkouts then, since every command line differs.
And a build directory that is moved or copied silently reads the old checkout's data.
The pattern that replaces one is `sc_target_source_paths` in CMake and `nx::source_path` in code (nexus/source_root.hh).

The gate reads what configure wrote: compile_commands.json for the definitions, and the generated sources it lists.
A source a build step generates is checked once it exists, so a fresh build directory sees it after its first build.
"""

from __future__ import annotations

import fnmatch
import json
import os
import shlex
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Exception_:
    """One known absolute path, allowed until the issue that removes it closes.

    `macro` names a definition, `file` a glob over generated sources relative to the build directory; exactly one is set.
    """

    issue: str
    macro: str = ""
    file: str = ""


# Shaped-core's own, each in a shipped library and each a design question rather than a mechanical fix.
# A consumer's build compiles these libraries too, so its gate passes them along with its own.
# Each marker below names its issue, so closing the issue fails the issues gate until the entry goes with it.
SHAPED_CORE_EXCEPTIONS = (
    # TEMPORARY(debt-oidn-weights-source-tree-path)
    Exception_(issue="debt-oidn-weights-source-tree-path", macro="SR_OIDN_WEIGHTS_DIR"),
    # TEMPORARY(debt-dx12-agility-source-tree-fallback)
    Exception_(issue="debt-dx12-agility-source-tree-fallback", macro="SG_AGILITY_RUNTIME_DIR"),
    # TEMPORARY(debt-slib-package-source-dir-absolute)
    Exception_(issue="debt-slib-package-source-dir-absolute", file="*/shader-packages/*"),
)

# What a generated source may be; anything else under the build directory is not read.
_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp"}


@dataclass(frozen=True)
class Finding:
    """One absolute path: in a definition (`macro` set) or in a generated source (`file` set)."""

    value: str
    macro: str = ""
    file: str = ""
    example: str = ""


def _spellings(path: Path) -> list[str]:
    """`path` as a search key: forward slashes, lower case, no trailing separator."""
    text = str(path).replace("\\", "/").rstrip("/").lower()
    return [text] if text else []


def _normalized(text: str) -> str:
    return text.replace("\\\\", "/").replace("\\", "/").lower()


def _arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    command = entry.get("command", "")
    # Windows command lines keep their backslashes; shlex in posix mode would eat them.
    return shlex.split(command, posix=os.name != "nt")


def _definitions(arguments: list[str]) -> list[tuple[str, str]]:
    """Each `-DNAME=VALUE` or `/DNAME=VALUE` as (NAME, VALUE), the value as written."""
    out = []
    i = 0
    while i < len(arguments):
        a = arguments[i]
        body = None
        if a in ("-D", "/D") and i + 1 < len(arguments):
            body = arguments[i + 1]
            i += 1
        elif a.startswith(("-D", "/D")) and len(a) > 2:
            body = a[2:]
        if body is not None and "=" in body:
            name, _, value = body.partition("=")
            out.append((name, value))
        i += 1
    return out


def _is_under(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def find(build_dir: Path, source_root: Path) -> list[Finding]:
    """Every absolute source or build path in `build_dir`'s definitions and generated sources, one finding per macro and file."""
    database = build_dir / "compile_commands.json"
    entries = json.loads(database.read_text(encoding="utf-8"))
    roots = _spellings(source_root.resolve()) + _spellings(build_dir.resolve())
    build = build_dir.resolve()

    found: dict[tuple[str, str], Finding] = {}
    generated_dirs: set[Path] = set()
    for entry in entries:
        file = Path(entry.get("directory", ".")) / entry["file"]
        for name, value in _definitions(_arguments(entry)):
            if any(r in _normalized(value) for r in roots):
                found.setdefault(("macro", name), Finding(value=value, macro=name, example=str(file)))
        resolved = file.resolve()
        # CMake's own files (compiler probes) are its business, not a generator of ours.
        if _is_under(resolved, build) and "CMakeFiles" not in resolved.parts:
            generated_dirs.add(resolved.parent)

    for directory in sorted(generated_dirs):
        # a directory a build step has yet to write is checked after that build
        if not directory.is_dir():
            continue
        for path in sorted(directory.iterdir()):
            if path.suffix.lower() not in _SOURCE_SUFFIXES or not path.is_file():
                continue
            text = _normalized(path.read_text(encoding="utf-8", errors="replace"))
            hit = next((r for r in roots if r in text), None)
            if hit is not None:
                rel = path.relative_to(build).as_posix()
                found.setdefault(("file", rel), Finding(value=hit, file=rel))
    return list(found.values())


def _allowed(f: Finding, exceptions: tuple[Exception_, ...]) -> Exception_ | None:
    for e in exceptions:
        if f.macro and e.macro == f.macro:
            return e
        if f.file and e.file and fnmatch.fnmatch(f.file, e.file):
            return e
    return None


def report(findings: list[Finding], exceptions: tuple[Exception_, ...], write) -> bool:
    """Print the findings, `write` taking one line each; True when every one is an allowed exception."""
    ok = True
    for f in sorted(findings, key=lambda f: (f.macro or "~", f.file)):
        known = _allowed(f, exceptions)
        where = f"definition {f.macro}={f.value}" if f.macro else f"generated source {f.file}"
        if known is not None:
            write(f"abspaths: allowed until {known.issue}: {where}")
            continue
        ok = False
        write(f"abspaths: an absolute path in {where}" + (f" (e.g. compiling {f.example})" if f.example else ""))
    if not ok:
        write("abspaths: a binary must not carry a path of its checkout; pass the directory with sc_target_source_paths "
              "and join it with nx::source_path at run time (libs/base/nexus/src/nexus/source_root.hh)")
    return ok


def gate(build_dir: Path, source_root: Path, exceptions: tuple[Exception_, ...], write) -> bool:
    """The whole gate over one configured build directory: True when nothing but an allowed exception carries a path."""
    if not (build_dir / "compile_commands.json").is_file():
        write(f"abspaths: {build_dir} has no compile_commands.json; configure it first")
        return False
    return report(find(build_dir, source_root), exceptions, write)
