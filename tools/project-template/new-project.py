#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///

"""Lay out a new project consuming shaped-core: the build files, a dev.py, the house-style files and one sample library.

Run from the new project's root, with shaped-core already at extern/shaped-core:

    uv run extern/shaped-core/tools/project-template/new-project.py --name <project>

It writes only files that do not exist yet, and refuses before writing anything if one does, unless `--skip-existing` keeps those.
It also sets the submodule's `.gitmodules` entry to follow `main` and to merge on `git submodule update`.
What needs judgement, specializing CLAUDE.md and the skills above all, is docs/guides/starting-a-project.md's, not this script's.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / "template"
SHAPED_CORE = HERE.parents[1]

# Copied byte for byte from shaped-core's root: `.clang-format` is authoritative, and the house-style gate holds the copy to it.
VERBATIM = (".clang-format", ".editorconfig", ".clangd")

# Copied with its doc references pointed into the submodule; the project extends it from then on.
LINT_POLICY = ".shaped-lint.yml"

_NAME = re.compile(r"^[a-z][a-z0-9]*(-[a-z0-9]+)*$")
_NAMESPACE = re.compile(r"^[a-z][a-z0-9_]*$")


def _substitute(text: str, names: dict[str, str]) -> str:
    for key, value in names.items():
        text = text.replace(key, value)
    return text


def _sort_include_runs(text: str) -> str:
    """Each run of adjacent `#include <...>` lines in sorted order, as clang-format's regrouping leaves it once a name is substituted."""
    lines = text.split("\n")
    out: list[str] = []
    run: list[str] = []
    for line in [*lines, None]:
        if line is not None and line.startswith("#include <"):
            run.append(line)
            continue
        out += sorted(run)
        run = []
        if line is not None:
            out.append(line)
    return "\n".join(out)


def _plan(root: Path, names: dict[str, str]) -> list[tuple[Path, str]]:
    """Every file to write, as its destination and its text."""
    files: list[tuple[Path, str]] = []
    for src in sorted(TEMPLATE.rglob("*")):
        if src.is_dir():
            continue
        rel = Path(_substitute(src.relative_to(TEMPLATE).as_posix(), names))
        if src.name == ".gitkeep":
            files.append((root / rel, ""))
        else:
            text = _substitute(src.read_text(encoding="utf-8"), names)
            if src.suffix in (".cc", ".hh"):
                text = _sort_include_runs(text)
            files.append((root / rel, text))
    # Read as text, so a checkout that left CRLF behind still writes LF, as .gitattributes asks.
    for name in VERBATIM:
        files.append((root / name, (SHAPED_CORE / name).read_text(encoding="utf-8")))
    policy = (SHAPED_CORE / LINT_POLICY).read_text(encoding="utf-8")
    policy = re.sub(r"(?<![\w/])(libs/base/clean-core/|tools/shaped-linter/)", r"extern/shaped-core/\1", policy)
    files.append((root / LINT_POLICY, policy))
    return files


def _configure_submodule(root: Path) -> None:
    """Point the shaped-core entry of `.gitmodules` at `main`, merging on update, where it does not say otherwise already."""
    if not (root / ".gitmodules").is_file():
        return
    for key, value in (("branch", "main"), ("update", "merge")):
        name = f"submodule.extern/shaped-core.{key}"
        current = subprocess.run(["git", "config", "-f", ".gitmodules", "--get", name], cwd=root, capture_output=True, text=True)
        if current.returncode == 0 and current.stdout.strip():
            continue
        subprocess.run(["git", "config", "-f", ".gitmodules", name, value], cwd=root, check=True)
        print(f".gitmodules: {name} = {value}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--name", required=True, help="the project's name, kebab-case: also its CMake project()")
    parser.add_argument("--lib", help="the sample library's name, kebab-case (default: the project's name)")
    parser.add_argument("--namespace", help="the sample library's namespace (default: the library's name, snake_case)")
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="the project's root (default: the current directory)")
    parser.add_argument("--skip-existing", action="store_true",
                        help="keep files that exist already, such as the README a new GitHub repository starts with, and write the rest")
    args = parser.parse_args()

    lib = args.lib or args.name
    namespace = args.namespace or lib.replace("-", "_")
    for value, pattern, what in ((args.name, _NAME, "--name"), (lib, _NAME, "--lib"), (namespace, _NAMESPACE, "--namespace")):
        if not pattern.match(value):
            print(f"new-project: {what} {value!r} is not {'snake_case' if what == '--namespace' else 'kebab-case'}", file=sys.stderr)
            return 2

    root = args.root.resolve()
    if not (root / "extern" / "shaped-core" / "dev.py").is_file():
        print(f"new-project: {root / 'extern' / 'shaped-core'} is not a shaped-core checkout; add the submodule first", file=sys.stderr)
        return 2

    names = {"__project__": args.name, "__lib__": lib, "__ns__": namespace}
    files = _plan(root, names)
    existing = [dst for dst, _ in files if dst.exists()]
    if existing and not args.skip_existing:
        print("new-project: these exist already, so nothing was written (--skip-existing keeps them and writes the rest):",
              file=sys.stderr)
        for dst in existing:
            print(f"  {dst.relative_to(root).as_posix()}", file=sys.stderr)
        return 1
    files = [(dst, text) for dst, text in files if not dst.exists()]
    for dst in existing:
        print(f"new-project: kept {dst.relative_to(root).as_posix()}, which existed already", file=sys.stderr)

    for dst, text in files:
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(text, encoding="utf-8", newline="\n")
        print(dst.relative_to(root).as_posix())
    _configure_submodule(root)
    print("new-project: done; extern/shaped-core/docs/guides/starting-a-project.md is what comes next", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
