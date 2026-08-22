"""Bundle build diagnostics and logs into zip archives for CI artifacts.

Two collectors, mirroring the two diagnostic trails dev.py leaves under build/<preset>/:

- diag sidecars (``*.diag.json``), written by diag-launcher next to each object or binary — the structured per-invocation compiler output build_diag reads;
- run logs (``run-logs/*``) plus the ``configure``/``build``/``test`` step sidecars and ``*.results.xml`` — the raw captured streams.
  They are the last resort, for when the structured sidecars do not explain a failure.

Archive entry names stay relative to the repo root, so extracting at the root reproduces ``build/<preset>/…`` and build_diag can be pointed straight at the result.
"""

from __future__ import annotations

import zipfile
from pathlib import Path

# dev.py's own per-preset step sidecars (distinct from CMake's own *.json).
_STEP_SIDECARS = ("configure.json", "build.json", "test.json")


def _zip(files: list[Path], output: Path, root: Path) -> int:
    """Zip `files` into `output`, each stored relative to `root`.

    Returns the number of files written, deduplicated and with missing files skipped.
    """
    unique = sorted({f for f in files if f.is_file()})
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in unique:
            try:
                arcname = f.relative_to(root).as_posix()
            except ValueError:
                arcname = f.name
            zf.write(f, arcname)
    return len(unique)


def archive_diag(build_dirs: list[Path], output: Path, root: Path) -> int:
    """Bundle every ``*.diag.json`` under the given build dirs into `output`."""
    files: list[Path] = []
    for d in build_dirs:
        if d.is_dir():
            files.extend(d.rglob("*.diag.json"))
    return _zip(files, output, root)


def archive_logs(build_root: Path, output: Path, root: Path) -> int:
    """Bundle captured run logs and step sidecars under `build_root` into `output`.

    Collects ``run-logs/*``, the per-preset step sidecars, ``*.results.xml``, and the ``*.ccrec``
    recordings nexus leaves beside them for a failing test.
    The diag sidecars are deliberately left out — `archive_diag` has them.

    The ``.ccrec`` files are what makes a remote-only failure diagnosable rather than guessable:
    a ``nx::config::recorded`` test that fails writes its whole event stream out, so the run's
    evidence comes back from a machine nobody can attach a debugger to.
    """
    files: list[Path] = []
    if build_root.is_dir():
        files.extend(build_root.rglob("run-logs/*"))
        files.extend(build_root.rglob("*.results.xml"))
        files.extend(build_root.rglob("*.ccrec"))
        for name in _STEP_SIDECARS:
            files.extend(build_root.rglob(name))
    return _zip(files, output, root)
