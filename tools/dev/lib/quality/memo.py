"""A cache of `check` verdicts, keyed on content rather than on git history.

Selection answers "could this change affect this gate"; the memo answers "has this gate already passed on exactly these inputs".
A gate whose key is unchanged since it last passed is skipped, and the run says so.

A watched gate's key is the blob id of every tracked file its globs match, with a dirty or untracked file hashed from disk instead, plus a stamp naming the tool that runs it.
A test binary's key is the hash of its artifact, which already folds in every source it compiled from, plus the preset.
An environment stamp is part of it too, the platform and the GPU driver, because a driver update is exactly the change git never sees.

Never memoized: an `always` gate, whose inputs are the whole tree, and a thorough trigger, which is a decision about this change.
`--all` and a gate named on the command line bypass the cache, so a deliberate full run stays full.

The cache lives under `build/`, and deleting it can never affect correctness: a miss is a run.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import shutil
import subprocess
from collections.abc import Callable, Iterable
from pathlib import Path

from ..core import profile
from .watch import Watch

_VERSION = 1


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _git(root: Path, args: list[str]) -> str | None:
    try:
        out = subprocess.run(["git", *args], cwd=str(root), capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return out.stdout if out.returncode == 0 else None


def file_ids(root: Path) -> dict[str, str] | None:
    """Every file in the working tree as repo-relative POSIX path -> content id; None when git cannot answer.

    A clean tracked file carries its blob id straight from the index, one git call for the whole tree.
    A dirty or untracked file is hashed from disk, and a deleted one is left out.
    """
    with profile.span("file ids", type="git"):
        listed = _git(root, ["ls-files", "-s", "-z"])
        status = _git(root, ["status", "--porcelain", "--untracked-files=all", "-z"])
    if listed is None or status is None:
        return None
    ids: dict[str, str] = {}
    for entry in listed.split("\0"):
        if not entry:
            continue
        # "<mode> <blob> <stage>\t<path>"
        meta, _, path = entry.partition("\t")
        parts = meta.split()
        if len(parts) >= 2 and path:
            ids[path] = parts[1]
    entries = [e for e in status.split("\0") if e]
    i = 0
    while i < len(entries):
        entry = entries[i]
        code, path = entry[:2], entry[3:]
        if "R" in code or "C" in code:
            i += 1  # the old name follows as its own entry
        i += 1
        full = root / path
        if full.is_file():
            ids[path] = "disk:" + _sha256_file(full)
        else:
            ids.pop(path, None)
    return ids


def environment_stamp() -> str:
    """What a test's result depends on beyond its own bytes: the platform, and the GPU driver where one can be asked."""
    parts = [platform.platform(), platform.machine()]
    parts.append(gpu_driver_version() or "")
    return hashlib.sha256("\n".join(parts).encode()).hexdigest()[:16]


def gpu_driver_version() -> str | None:
    """The installed GPU driver version(s), best effort; None where no probe applies or one fails."""
    try:
        if os.name == "nt":
            out = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command",
                 "(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + ' ' + $_.DriverVersion }) -join ';'"],
                capture_output=True, text=True, timeout=20)
            return out.stdout.strip() or None if out.returncode == 0 else None
        smi = shutil.which("nvidia-smi")
        if smi:
            out = subprocess.run([smi, "--query-gpu=driver_version", "--format=csv,noheader"],
                                 capture_output=True, text=True, timeout=20)
            return out.stdout.strip() or None if out.returncode == 0 else None
        nvidia = Path("/sys/module/nvidia/version")
        if nvidia.is_file():
            return nvidia.read_text(encoding="utf-8").strip() or None
    except (OSError, subprocess.SubprocessError):
        return None
    return None


class CheckMemo:
    """The verdict cache for one repo, loaded once per run and written after every pass."""

    def __init__(self, root: Path, path: Path, *, read: bool = True) -> None:
        self.root = root
        self.path = path
        self.read = read
        self._ids: dict[str, str] | None = None
        self._ids_loaded = False
        self._env: str | None = None
        self._data: dict = {"version": _VERSION, "gates": {}, "tests": {}}
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            if data.get("version") == _VERSION:
                self._data = data
        except (OSError, ValueError):
            pass

    # -- gates -------------------------------------------------------------

    def invalidate_files(self) -> None:
        """Forget the working tree's ids, after a fixer rewrote files."""
        self._ids_loaded = False
        self._ids = None

    def _file_ids(self) -> dict[str, str] | None:
        if not self._ids_loaded:
            self._ids = file_ids(self.root)
            self._ids_loaded = True
        return self._ids

    def gate_key(self, name: str, watch: Watch | None, stamp: Callable[[], str] | None) -> str | None:
        """The key for a watched gate, or None when it cannot be keyed and must run."""
        if watch is None:
            return None
        ids = self._file_ids()
        if ids is None:
            return None
        h = hashlib.sha256()
        h.update(f"gate:{name}\n".encode())
        h.update(((stamp() if stamp else "") + "\n").encode())
        for path in sorted(ids):
            if watch.matches(path):
                h.update(f"{path}\0{ids[path]}\n".encode())
        return h.hexdigest()

    def gate_passed_before(self, name: str, key: str | None) -> bool:
        return self.read and key is not None and self._data["gates"].get(name) == key

    def record_gate(self, name: str, key: str | None) -> None:
        if key is None:
            return
        self._data["gates"][name] = key
        self._save()

    def forget_gate(self, name: str) -> None:
        if self._data["gates"].pop(name, None) is not None:
            self._save()

    # -- tests -------------------------------------------------------------

    def _environment(self) -> str:
        if self._env is None:
            with profile.span("environment stamp", type="probe"):
                self._env = environment_stamp()
        return self._env

    def test_key(self, preset_name: str, artifact: Path) -> str | None:
        """The key for one test binary on one preset, or None when the artifact is not there to hash."""
        if not artifact.is_file():
            return None
        h = hashlib.sha256()
        h.update(f"test:{preset_name}\n{self._environment()}\n".encode())
        h.update(_sha256_file(artifact).encode())
        # An emscripten artifact is a .js beside its .wasm, and the code is in the .wasm.
        wasm = artifact.with_suffix(".wasm")
        if wasm.is_file():
            h.update(_sha256_file(wasm).encode())
        return h.hexdigest()

    def tests_passed_before(self, preset_name: str, artifacts: Iterable[tuple[str, Path]]) -> set[str]:
        """The binaries among `artifacts` (name, path) whose current key is recorded as passed."""
        if not self.read:
            return set()
        done: set[str] = set()
        for name, artifact in artifacts:
            key = self.test_key(preset_name, artifact)
            if key is not None and self._data["tests"].get(f"{preset_name}/{name}") == key:
                done.add(name)
        return done

    def record_tests(self, preset_name: str, passed: Iterable[tuple[str, Path]], failed: Iterable[str]) -> None:
        for name in failed:
            self._data["tests"].pop(f"{preset_name}/{name}", None)
        for name, artifact in passed:
            key = self.test_key(preset_name, artifact)
            if key is not None:
                self._data["tests"][f"{preset_name}/{name}"] = key
        self._save()

    # -- storage -----------------------------------------------------------

    def _save(self) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(self._data, indent=1, sort_keys=True), encoding="utf-8")
            os.replace(tmp, self.path)
        except OSError:
            pass  # a cache that cannot be written is a cache that misses next time
