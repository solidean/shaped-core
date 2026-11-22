#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///

"""The project template's suite: a project laid out by new-project.py configures, builds, tests and passes `check --all`.

The project lives in a throwaway directory, with this checkout linked in as its extern/shaped-core, so it tests the working tree rather than a commit.
A cold build of clean-core and nexus is most of its minutes, which is why it is a gate watching the template alone.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SHAPED_CORE = HERE.parents[1]


def _link(link: Path, target: Path) -> None:
    """A directory link: a junction on Windows, which needs no privilege, a symlink elsewhere."""
    if sys.platform == "win32":
        import _winapi

        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)


def _run(argv: list[str], cwd: Path) -> None:
    print(f"template-self-test: {' '.join(argv)}", flush=True)
    result = subprocess.run(argv, cwd=cwd)
    if result.returncode != 0:
        raise SystemExit(f"template-self-test: FAILED: `{' '.join(argv)}` exited {result.returncode} in {cwd}")


def main() -> int:
    # A short root: Windows caps an object file's path, and CMake's try_compile nests deep below the build directory.
    base = Path(tempfile.mkdtemp(prefix="sc-tpl-"))
    link = base / "extern" / "shaped-core"
    try:
        link.parent.mkdir(parents=True)
        _link(link, SHAPED_CORE)
        git = ["git", "-c", "user.name=template-self-test", "-c", "user.email=template-self-test@invalid"]
        _run(["git", "init", "-q", "-b", "main"], base)
        _run(["uv", "run", str(link / "tools" / "project-template" / "new-project.py"),
              "--name", "sample-app", "--lib", "sample-core", "--namespace", "sample"], base)
        # `check` scopes itself against a base, so the project needs a commit; the linked checkout stays out of it.
        (base / ".git" / "info" / "exclude").write_text("extern/\n", encoding="utf-8")
        _run([*git, "add", "-A"], base)
        _run([*git, "commit", "-q", "-m", "initial layout"], base)
        _run(["uv", "run", "dev.py", "check", "--all"], base)
        print("template-self-test: OK")
        return 0
    finally:
        # The link first, so removing the tree never reaches into the checkout it points at.
        if link.is_junction() or link.is_symlink():
            os.rmdir(link) if sys.platform == "win32" else link.unlink()
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
