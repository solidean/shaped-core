"""Bundle build diagnostics and logs into zip archives for CI artifacts.

Two collectors, mirroring the two diagnostic trails dev.py leaves under
build/<preset>/:

- diag sidecars (``*.diag.json``) written by diag-launcher next to each object /
  binary — the structured per-invocation compiler/linker output build_diag
  reads. This is the build-step analogue of the JUnit XML test report.
- run logs (``run-logs/*``) plus the ``configure``/``build``/``test`` JSON step
  sidecars and ``*.results.xml`` — the raw captured streams, a last resort when
  the structured sidecars don't explain a failure.

Archive entry names stay relative to the repo root, so extracting an archive at
the repo root reproduces ``build/<preset>/…`` and build_diag can be pointed at
the result directly.
"""

from __future__ import annotations

import zipfile
from pathlib import Path

# dev.py's own per-preset step sidecars (distinct from CMake's own *.json).
_STEP_SIDECARS = ("configure.json", "build.json", "test.json")


def _zip(files: list[Path], output: Path, root: Path) -> int:
    """Zip `files` into `output`, each stored relative to `root`. Returns the
    number of files written (deduplicated; missing files skipped)."""
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

    Collects ``run-logs/*``, the per-preset step sidecars, and any
    ``*.results.xml``. The diag sidecars are intentionally left out — they have
    their own archive (see `archive_diag`)."""
    files: list[Path] = []
    if build_root.is_dir():
        files.extend(build_root.rglob("run-logs/*"))
        files.extend(build_root.rglob("*.results.xml"))
        for name in _STEP_SIDECARS:
            files.extend(build_root.rglob(name))
    return _zip(files, output, root)
