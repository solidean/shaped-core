"""A second worktree of the checkout at another commit, submodules included, for running a test where a change began.

`ship`'s proof asks it of a red test the change may have caused: red at the change's base too, the change did not cause it.
The worktree lives at `build/proof-base/` (gitignored) and is kept between ships, so its build dir stays warm.
Each submodule is a `git worktree` of the checkout's own submodule repository, so nothing is cloned or fetched;
a submodule the checkout never initialized is left out, as the checkout's own build does without it.

Public API:
    ensure(root, commit)   the worktree at `commit`, or the reason it could not be made
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

DIR = Path("build") / "proof-base"


def _git(repo: Path, *argv: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *argv], capture_output=True, text=True, encoding="utf-8", errors="replace")


def _gitlinks(repo: Path) -> list[tuple[str, str]]:
    """The submodules `repo`'s HEAD records: (path, commit)."""
    out = _git(repo, "ls-tree", "-r", "HEAD")
    links = []
    for line in out.stdout.splitlines():
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) == 3 and parts[0] == "160000":
            links.append((path, parts[2]))
    return links


def _checkout(main: Path, wt: Path, commit: str) -> str | None:
    """`wt` as a worktree of the repo at `main`, detached at `commit`; None, or why it failed."""
    if (wt / ".git").exists():
        r = _git(wt, "checkout", "--detach", "--force", "--quiet", commit)
    else:
        wt.mkdir(parents=True, exist_ok=True)
        _git(main, "worktree", "prune")
        r = _git(main, "worktree", "add", "--detach", "--force", str(wt), commit)
    if r.returncode != 0:
        return f"git worktree at {commit[:12]} in {wt}: {(r.stderr or r.stdout).strip()}"
    return None


def _submodules(main: Path, wt: Path) -> str | None:
    """Every submodule `main` has initialized, as a worktree of its repository under `wt` at the commit `wt` records; nested ones too."""
    for path, commit in _gitlinks(wt):
        sub_main = main / path
        if not (sub_main / ".git").exists():
            continue
        if _git(sub_main, "cat-file", "-e", f"{commit}^{{commit}}").returncode != 0:
            return f"{path}: commit {commit[:12]} is not in the checkout's submodule; `dev.py submodules sync` first"
        if why := _checkout(sub_main, wt / path, commit):
            return why
        if why := _submodules(sub_main, wt / path):
            return why
    return None


def _link_fetched(root: Path, wt: Path) -> bool:
    """Each dependency the checkout fetched (`extern/<dep>/.install`, nested ones too) as a junction to the checkout's copy,
    where the worktree pins the same version: its `dependency.yml` reads the same.
    A dependency pinned otherwise stays unfetched there.
    Whether any link came or went: a build dir configured before then lacks, or wrongly has, what that dependency enables."""
    changed = False
    for install in [*root.glob("extern/*/.install"), *root.glob("extern/*/extern/*/.install")]:
        rel = install.relative_to(root)
        pin, here = install.parent / "dependency.yml", wt / rel
        there = wt / rel.parent / "dependency.yml"
        same = _pin(pin) is not None and _pin(pin) == _pin(there)
        if here.is_dir() and not same:
            _unlink(here)
            changed = True
        if same and not here.exists():
            changed = True
            if os.name == "nt":
                # A junction needs no privilege, where a symlink does.
                subprocess.run(["cmd", "/c", "mklink", "/J", str(here), str(install)], capture_output=True)
            else:
                here.symlink_to(install, target_is_directory=True)
    return changed


def _pin(dependency_yml: Path) -> str | None:
    """A dependency's pin: its `pin_hash:` line, which tools/dev/lib/pipeline/prereqs.py reads too; else the whole file, line ends aside."""
    try:
        lines = dependency_yml.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    whole = "\n".join(ln.rstrip() for ln in lines)
    return next((ln.strip() for ln in lines if ln.strip().startswith("pin_hash:")), whole)


def _unlink(link: Path) -> None:
    """Removes a junction or symlink, never what it points at; a real directory stays."""
    if os.name == "nt":
        subprocess.run(["cmd", "/c", "rmdir", str(link)], capture_output=True)  # removes a junction, or an empty dir only
    elif link.is_symlink():
        link.unlink()


def ensure(root: Path, commit: str) -> tuple[Path | None, str]:
    """The base worktree of the checkout at `root`, detached at `commit` with its submodules; else None and why."""
    wt = root / DIR
    if why := _checkout(root, wt, commit):
        return None, why
    if why := _submodules(root, wt):
        return None, why
    if _link_fetched(root, wt):
        # Configured without a dependency, a build dir builds without what it enables (DXC's compiler, say), and a test then fails there for
        # that alone; configure only notices a missing build dir, so the whole of it goes.
        shutil.rmtree(wt / "build", ignore_errors=True)
    # The checkout's dev.toml, for the server and the toolchain, without what would make the worktree a session of its own.
    toml = root / "dev.toml"
    if toml.is_file():
        keep = [ln for ln in toml.read_text(encoding="utf-8").splitlines()
                if not ln.strip().startswith(("speculative_tests", "ship_proof"))]
        (wt / "dev.toml").write_text("\n".join(keep) + "\n", encoding="utf-8")
    return wt, ""
