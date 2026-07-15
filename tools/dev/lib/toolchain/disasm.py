"""Symbol search and disassembly over built object files (a local godbolt).

The linked release .exe on Windows is stripped of its COFF symbol table (symbols
live in a PDB that release builds don't emit), so the reliable, config-independent
place to find symbols is the **object files**: every .obj keeps a full mangled
symbol table, and for a CC_FORCE_INLINE-heavy hot function the .obj already holds
its real, fully-inlined codegen. We therefore scan objects, grouped by the CMake
target that owns them (`.../CMakeFiles/<target>.dir/...`).

Demangling uses `llvm-nm --demangle` rather than a standalone demangler: LLVM's
built-in demangler handles both the Itanium and the MSVC ABI, while the
`llvm-undname`/`llvm-cxxfilt` binaries are often not installed. A plain and a
`--demangle` pass over the same files list symbols in identical order, so we zip
them to pair every mangled name with its readable form.

Caveat: object code is pre-link, so cross-object call targets are unresolved
placeholders. Local control flow (loops, in-function branches) resolves fine —
which is what reading a hot loop needs.
"""

from __future__ import annotations

import fnmatch
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..core.process import response_file
from .llvm_tools import resolve_tool

# Object-file suffixes across toolchains (MSVC/clang-cl emit .obj; gcc/clang .o).
_OBJ_SUFFIXES = (".obj", ".o")

# nm symbol line: "<addr> [<size>] <type> <name>". Size present with --print-size.
_NM_LINE = re.compile(r"^\s*([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?(\S)\s+(.+?)\s*$")
# A file header line in multi-file nm/objdump output ends the path with ':'.
_FILE_HEADER = re.compile(r"^(.*\.(?:obj|o)):\s*$", re.IGNORECASE)

# nm type letters for code/text symbols (the disassemblable ones).
_TEXT_TYPES = frozenset("tT")


class DisasmError(Exception):
    """A tool is missing or a subprocess failed in a way the command should report."""


@dataclass(frozen=True)
class Symbol:
    """One defined symbol found in an object file."""

    target: str  # owning CMake target (from the .dir path segment)
    obj: Path  # object file the symbol is defined in
    mangled: str  # linkage name (what --disassemble-symbols takes)
    demangled: str  # readable form, or == mangled if not manglable
    sym_type: str  # nm type letter (T/t = text, r/d/b = data, ...)
    size: int  # symbol size in bytes (0 when nm can't tell, e.g. COMDAT)

    @property
    def is_text(self) -> bool:
        return self.sym_type in _TEXT_TYPES


def find_tool(name: str, build_dir: Path) -> str:
    """Locate an llvm-* tool beside the configured compiler (or on PATH); raise if absent."""
    env_var = "LLVM_" + name.replace("llvm-", "").replace("-", "_").upper()  # llvm-objdump -> LLVM_OBJDUMP
    found = resolve_tool(name, env_var, build_dir)
    if not found:
        raise DisasmError(
            f"{name} not found. It ships with LLVM (beside clang); install LLVM or set {env_var}."
        )
    return found


def _target_of(obj: Path) -> str:
    """Recover the owning target from a '.../CMakeFiles/<target>.dir/...' object path."""
    for part in obj.parts:
        if part.endswith(".dir"):
            return part[: -len(".dir")]
    return "?"


def discover_objects(build_dir: Path, target_patterns: list[str] | None) -> dict[str, list[Path]]:
    """Map target -> its object files under build_dir, filtered by fnmatch patterns.

    `target_patterns` None/empty means every target. Patterns are matched against
    the derived target name (comma-splitting is the caller's job).
    """
    by_target: dict[str, list[Path]] = {}
    for suffix in _OBJ_SUFFIXES:
        for obj in build_dir.rglob("*" + suffix):
            by_target.setdefault(_target_of(obj), []).append(obj)

    if target_patterns:
        kept = {}
        for tgt, objs in by_target.items():
            if any(tgt == p or fnmatch.fnmatch(tgt, p) for p in target_patterns):
                kept[tgt] = objs
        by_target = kept
    return dict(sorted(by_target.items()))


def _run_nm(nm: str, objs: list[Path], demangle: bool) -> dict[Path, list[str]]:
    """Run nm over all objs at once; return per-file lists of raw symbol lines."""
    cmd = [nm, "--defined-only", "--print-size"]
    if demangle:
        cmd.append("--demangle")
    # a target's object list runs to hundreds of absolute paths, past the Windows
    # command-line limit; response_file spills to `@rsp` when it would not fit.
    try:
        with response_file([str(o) for o in objs], prefix="llvm-nm-") as tail:
            proc = subprocess.run(cmd + tail, capture_output=True, text=True, encoding="utf-8", errors="replace")
    except OSError as e:
        raise RuntimeError(f"could not run {nm}: {e}") from e
    # nm exits non-zero if some file has no symbols; that's not fatal for a scan.

    per_file: dict[Path, list[str]] = {}
    current: Path | None = None
    single = objs[0] if len(objs) == 1 else None  # single-file nm omits the header
    for line in proc.stdout.splitlines():
        header = _FILE_HEADER.match(line)
        if header:
            current = Path(header.group(1))
            per_file.setdefault(current, [])
            continue
        target_file = current or single
        if target_file is not None and line.strip():
            per_file.setdefault(target_file, []).append(line)
    return per_file


def enumerate_symbols(nm: str, by_target: dict[str, list[Path]]) -> list[Symbol]:
    """List every defined symbol across the targets' objects, mangled + demangled."""
    all_objs = [o for objs in by_target.values() for o in objs]
    if not all_objs:
        return []
    obj_target = {o: t for t, objs in by_target.items() for o in objs}

    mangled = _run_nm(nm, all_objs, demangle=False)
    demangled = _run_nm(nm, all_objs, demangle=True)

    symbols: list[Symbol] = []
    for obj, mlines in mangled.items():
        dlines = demangled.get(obj, [])
        for i, mline in enumerate(mlines):
            mm = _NM_LINE.match(mline)
            if not mm:
                continue
            _addr, size, sym_type, mname = mm.groups()
            dname = mname
            if i < len(dlines):
                dm = _NM_LINE.match(dlines[i])
                if dm:
                    dname = dm.group(4)
            symbols.append(
                Symbol(
                    target=obj_target.get(obj, _target_of(obj)),
                    obj=obj,
                    mangled=mname,
                    demangled=dname,
                    sym_type=sym_type,
                    size=int(size, 16) if size else 0,
                )
            )
    return symbols


def match_symbols(
    symbols: list[Symbol], pattern: str, *, regex: bool, text_only: bool
) -> list[Symbol]:
    """Filter symbols whose mangled OR demangled form matches `pattern`.

    Substring (case-insensitive) by default; a full regex with --regex. `text_only`
    keeps just code symbols (the disassemblable ones).
    """
    if regex:
        rx = re.compile(pattern, re.IGNORECASE)
        hit = lambda s: bool(rx.search(s.mangled) or rx.search(s.demangled))
    else:
        needle = pattern.lower()
        hit = lambda s: needle in s.mangled.lower() or needle in s.demangled.lower()
    return [s for s in symbols if (not text_only or s.is_text) and hit(s)]


def disassemble(
    objdump: str,
    obj: Path,
    mangled: str,
    *,
    intel: bool = True,
    source: bool = False,
    show_bytes: bool = False,
) -> str:
    """Disassemble a single symbol out of an object file; return objdump's text.

    `-r` interleaves relocations so cross-object call/RIP targets (unresolved in
    object code) show their real symbol. We can't add `-C` to demangle them:
    `--disassemble-symbols` matches the raw table, so demangling it there makes the
    lookup miss. Reloc targets therefore stay mangled (the function name still leads).
    """
    cmd = [objdump, "-d", "-r", f"--disassemble-symbols={mangled}"]
    cmd.append("--x86-asm-syntax=intel" if intel else "--x86-asm-syntax=att")
    if not show_bytes:
        cmd.append("--no-show-raw-insn")
    if source:
        cmd += ["-S", "--line-numbers"]
    cmd.append(str(obj))
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0 and not proc.stdout:
        raise DisasmError(f"objdump failed for {mangled}:\n{proc.stderr.strip()}")
    return proc.stdout
