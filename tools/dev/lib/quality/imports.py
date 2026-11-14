"""The repository files a Python script imports, transitively: what a self-test's gate watches.

Read from the source, never by importing it, so a gate's watch set costs a parse of each file and nothing runs.
A dotted module resolves against the repository root, which every script here puts on sys.path, and relative imports
against the importing package; anything outside the repository, the standard library and third-party packages, is
nobody's to watch.

The dev facade (`tools/dev/__init__.py`) imports nothing itself: each `dev.X` loads its module on first use.
So a use of the facade is followed by name, through the facade's own `_LAZY` table: `dev.run_step` reaches
lib/core/process.py, and a script that never touches `dev.X` does not depend on every module the facade can reach.
A module loaded some other way, by path through importlib say, is no import and has to be watched by hand.
"""

from __future__ import annotations

import ast
import functools
from pathlib import Path

FACADE = "tools/dev/__init__.py"


def closure(root: Path, script: str) -> frozenset[str]:
    """`script` and every repository file it imports, transitively, as root-relative POSIX paths."""
    return _closure(root.resolve(), script)


@functools.cache
def _closure(root: Path, script: str) -> frozenset[str]:
    facade = _facade_table(root)
    seen: set[Path] = set()
    todo = [root / script]
    while todo:
        f = todo.pop()
        if f in seen or not f.is_file():
            continue
        seen.add(f)
        todo += _imports_of(root, f, facade)
    return frozenset(p.relative_to(root).as_posix() for p in seen)


@functools.cache
def _facade_table(root: Path) -> dict[str, str]:
    """The facade's names, each with the dotted module behind it, absolute."""
    path = root / FACADE
    if not path.is_file():
        return {}
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        target = node.target if isinstance(node, ast.AnnAssign) else node.targets[0] if isinstance(node, ast.Assign) else None
        if isinstance(target, ast.Name) and target.id == "_LAZY" and isinstance(node.value, ast.Dict):
            table = {}
            for key, value in zip(node.value.keys, node.value.values):
                module, attr = (ast.literal_eval(e) for e in value.elts)
                dotted = "tools.dev" + module if module.startswith(".") else module
                table[ast.literal_eval(key)] = f"{dotted}.{attr}"
            return table
    return {}


def _module_file(root: Path, dotted: str) -> Path | None:
    """The file a dotted name is, a module or a package, or None outside the repository."""
    base = root.joinpath(*dotted.split("."))
    for candidate in (base.with_suffix(".py"), base / "__init__.py"):
        if candidate.is_file():
            return candidate
    return None


def _package_of(root: Path, f: Path) -> list[str]:
    """The package a relative import in `f` starts from: its directory, for a module and an `__init__.py` alike."""
    return list(f.relative_to(root).parent.parts)


def _imports_of(root: Path, f: Path, facade: dict[str, str]) -> list[Path]:
    try:
        tree = ast.parse(f.read_text(encoding="utf-8"))
    except (SyntaxError, UnicodeDecodeError):
        return []
    package = _package_of(root, f) if f.is_relative_to(root) else []
    dotted: list[str] = []
    facade_aliases: set[str] = set()

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                dotted.append(a.name)
                if a.name == "tools.dev" and a.asname:
                    facade_aliases.add(a.asname)
        elif isinstance(node, ast.ImportFrom):
            base = node.module or ""
            if node.level:
                parent = package[: len(package) - (node.level - 1)] if node.level > 1 else package
                base = ".".join([*parent, *([base] if base else [])])
            dotted.append(base)
            for a in node.names:
                name = f"{base}.{a.name}" if base else a.name
                if name == "tools.dev":
                    facade_aliases.add(a.asname or a.name)
                elif base == "tools.dev" and a.name in facade:
                    dotted.append(facade[a.name])
                else:
                    dotted.append(name)

    # A facade use reaches its module by name: `dev.run_step`, or `dev.console.dim(...)`.
    if facade_aliases:
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id in facade_aliases:
                if node.attr in facade:
                    dotted.append(facade[node.attr])

    out: list[Path] = []
    for name in dotted:
        # A package's __init__ runs before anything below it does.
        parts = name.split(".") if name else []
        for i in range(1, len(parts) + 1):
            m = _module_file(root, ".".join(parts[:i]))
            if m is not None:
                out.append(m)
    return out
