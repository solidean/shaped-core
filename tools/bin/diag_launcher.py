#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# ///
"""Stdlib-only Python port of the checked-in ``diag-launcher.exe``.

A minimal compile/link launcher for CMake's ``CMAKE_<LANG>_COMPILER_LAUNCHER`` / ``CMAKE_<LANG>_LINKER_LAUNCHER``.
It runs a wrapped compiler or linker, captures ``stdout``, ``stderr``, timing and the exit code, and writes a per-invocation ``.diag.json`` sidecar next to the produced output.
It does *not* parse diagnostics — it persists raw per-invocation output and metadata for ``build_diag`` to read later.

The two launchers are used on different platforms and ``build_diag`` reads both.
So the sidecar schema (``SCHEMA_VERSION`` below), the CLI, the output-path detection and the launcher-peeling rules must stay identical between them.
Only this half of that contract is checkable in-repo: the ``.exe``'s Rust source lives outside this tree, which is why the notes below appeal to Rust behaviour they cannot cite.
``diag_launcher.sh`` is the wrapper the POSIX presets point at, and docs/guides/ci.md covers which preset gets which.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from datetime import datetime, timezone

SCHEMA_VERSION = 2

# Known compiler-launcher wrappers that get peeled off the front of the command
# so the sidecar's ``tool`` field reports the actual compiler, not the cache.
KNOWN_LAUNCHERS = frozenset(
    {"sccache", "ccache", "buildcache", "distcc", "icecc", "icerun"}
)


def split_lines(data: bytes) -> list[str]:
    """Decode captured output lossily and split it into logical lines with no trailing terminators.

    Mirrors Rust ``str::lines``: split on ``\\n`` only, strip a single trailing ``\\r`` per line, and never yield a trailing empty element for newline-terminated output.
    Deliberately *not* ``str.splitlines()``, which also breaks on ``\\v``, ``\\f``, ``\\x1c`` and a lone ``\\r`` — every one of which would diverge from the .exe.
    """
    text = data.decode("utf-8", "replace")
    if text == "":
        return []
    parts = text.split("\n")
    # A trailing "\n" leaves an empty final element; drop it like Rust does.
    if parts and parts[-1] == "":
        parts.pop()
    # Strip one trailing "\r" per line so "\r\n" normalizes to "\n".
    return [p[:-1] if p.endswith("\r") else p for p in parts]


def rfc3339(dt: datetime) -> str:
    """Format a UTC datetime as RFC 3339 with exactly 6 fractional digits and a trailing ``Z``, matching Rust ``SecondsFormat::Micros``.

    ``isoformat`` drops the fraction when it is zero, so the formatting is done by hand.
    """
    return dt.strftime("%Y-%m-%dT%H:%M:%S.%f") + "Z"


def sidecar_path(output: str) -> str:
    """Append ``.diag.json`` to the output path, preserving the original extension (``build/foo.o`` -> ``build/foo.o.diag.json``)."""
    return output + ".diag.json"


def _stem(name: str) -> str:
    """File stem of a path, OS-independent: split on both ``/`` and ``\\``, take the last component, then strip everything from the last ``.``.

    Done by hand rather than via ``pathlib`` so separator handling does not depend on the host OS, and ``C:/tools/sccache.exe`` yields ``sccache`` everywhere.
    """
    base = name.replace("\\", "/").rsplit("/", 1)[-1]
    dot = base.rfind(".")
    return base[:dot] if dot > 0 else base


def is_known_launcher(name: str) -> bool:
    """Case-insensitive check of a command's stem against the known-launcher set."""
    return _stem(name).lower() in KNOWN_LAUNCHERS


def resolve_reported_tool(tool: str, child_args: list[str]) -> str:
    """If ``tool`` is a known launcher (sccache, ccache, ...), report the first non-launcher argument as the actual compiler; otherwise report ``tool``."""
    if is_known_launcher(tool):
        for arg in child_args:
            if not is_known_launcher(arg):
                return arg
    return tool


def find_output_path(args: list[str]) -> str | None:
    """Identify the produced output file from compiler/linker arguments.

    Recognizes Clang/GCC ``-o <path>`` and ``-o<path>``, MSVC ``cl.exe`` ``/Fo<path>`` / ``/Fo:<path>`` (plus ``/Fe`` for link-as-driver), and MSVC ``link.exe`` ``/OUT:<path>``, case-insensitively.
    Returns ``None`` when no output path can be determined.
    """
    skip_next = False
    for i, a in enumerate(args):
        # Arguments following -Xclang are passthrough to the clang frontend, and must not be interpreted as output flags.
        if skip_next:
            skip_next = False
            continue
        if a == "-Xclang":
            skip_next = True
            continue

        if a == "-o":
            if i + 1 < len(args):
                return args[i + 1]
            continue

        # MSVC-style flags use '/', so never match on a '-' prefix here — GCC flags like -fopenmp would collide with /Fo.
        if a.startswith("/"):
            body = a[1:]
            for prefix in ("OUT:", "Fo:", "Fe:", "Fo", "Fe"):
                if len(body) > len(prefix):
                    head, tail = body[: len(prefix)], body[len(prefix) :]
                    if head.lower() == prefix.lower():
                        return tail

        # GCC/Clang joined form: -o<path>.
        # The path portion must contain a '.' or a path separator, so a flag like -openmp does not match.
        if len(a) > 2 and a.startswith("-o"):
            tail = a[2:]
            if "." in tail or "/" in tail or "\\" in tail:
                return tail
    return None


def resolve_exit_code(rc: int) -> int:
    """Map a ``subprocess`` return code to the Rust convention.

    ``subprocess`` returns a negative value when the child was killed by a POSIX signal, which Rust reports as ``128 + signal``; otherwise the code passes through.
    """
    if rc < 0:
        return 128 + (-rc)
    return rc


def main(argv: list[str]) -> int:
    # argv[0] is this script; need at least a tool name after it.
    raw = list(argv[1:])
    if len(raw) < 1:
        sys.stderr.write("usage: diag-launcher <tool> [args...]\n")
        return 2
    tool = raw.pop(0)
    child_args = raw

    cwd = None
    try:
        cwd = os.getcwd()
    except OSError:
        cwd = None
    started_at = datetime.now(timezone.utc)

    try:
        proc = subprocess.run([tool, *child_args], capture_output=True)
    except OSError as e:
        sys.stderr.write(f"diag-launcher: failed to spawn '{tool}': {e}\n")
        return 127

    finished_at = datetime.now(timezone.utc)
    duration_ms = int(
        (finished_at - started_at).total_seconds() * 1000
    )

    exit_code = resolve_exit_code(proc.returncode)
    output_path = find_output_path(child_args)

    # Only persist a sidecar when we can place it next to a known output file.
    if output_path is not None:
        sidecar = sidecar_path(output_path)
        parent = os.path.dirname(sidecar)
        if parent:
            try:
                os.makedirs(parent, exist_ok=True)
            except OSError:
                pass

        record = {
            "schema_version": SCHEMA_VERSION,
            "tool": resolve_reported_tool(tool, child_args),
            "cwd": cwd,
            "argv": [tool, *child_args],
            "exit_code": exit_code,
            "started_at": rfc3339(started_at),
            "finished_at": rfc3339(finished_at),
            "duration_ms": duration_ms,
            "output_path": output_path,
            "stdout": split_lines(proc.stdout),
            "stderr": split_lines(proc.stderr),
        }

        # A failing sidecar write must never turn a successful build into a failed one, so warn and keep the wrapped tool's exit code.
        try:
            with open(sidecar, "w", encoding="utf-8") as f:
                json.dump(record, f, indent=2, ensure_ascii=False)
        except OSError as e:
            sys.stderr.write(
                f"diag-launcher: failed to write sidecar {sidecar}: {e}\n"
            )

    # Replay the wrapped tool's raw output verbatim, then propagate its exit code, masked to a single byte as the OS process exit code is.
    sys.stdout.buffer.write(proc.stdout)
    sys.stdout.buffer.flush()
    sys.stderr.buffer.write(proc.stderr)
    sys.stderr.buffer.flush()

    return exit_code & 0xFF


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
