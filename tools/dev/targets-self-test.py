#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for which executables target discovery refuses as unregistered nexus binaries — tools/dev/lib/project/targets.py.

The refusal and the manifest's writer, libs/base/nexus/cmake/NexusBinaries.cmake, have to agree on what registered means.
A test binary `all` does not build is registered but keeps no kinds in `binaries`, and refusing it stops every dev.py run of a consumer that has one.

    uv run tools/dev/targets-self-test.py [-v]
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.core.models import Target  # noqa: E402
from tools.dev.lib.project.targets import _refuse_unregistered_nexus_binaries, load_nexus_registered  # noqa: E402

VERBOSE = "-v" in sys.argv


def refused(manifest: dict, targets: list[Target]) -> str | None:
    """What the refusal says for `targets` against `manifest`, or None when it lets them through."""
    with tempfile.TemporaryDirectory() as tmp:
        build_dir = Path(tmp)
        (build_dir / "nexus-binaries.json").write_text(json.dumps(manifest), encoding="utf-8")
        kinds = {name: tuple(k) for name, k in manifest.get("binaries", {}).items()}
        targets = [Target(name=t.name, kind=t.kind, artifact=None, nexus_kinds=kinds.get(t.name, ())) for t in targets]
        try:
            _refuse_unregistered_nexus_binaries(targets, build_dir, load_nexus_registered(build_dir))
        except SystemExit as e:
            return str(e.code)
        return None


def exe(name: str) -> Target:
    return Target(name=name, kind="EXECUTABLE", artifact=None)


CASES = [
    (
        "a registered test binary passes",
        {"binaries": {"a-test": ["tests"]}, "paths": {"a-test": "/x/a-test"}},
        [exe("a-test")],
        None,
    ),
    (
        "a test binary `all` does not build is in `paths` only, and passes",
        {"binaries": {}, "paths": {"excluded-test": "/x/excluded-test"}},
        [exe("excluded-test")],
        None,
    ),
    (
        "a test binary that never called sc_nexus_binary is refused",
        {"binaries": {"a-test": ["tests"]}, "paths": {"a-test": "/x/a-test"}},
        [exe("a-test"), exe("forgotten-test")],
        "forgotten-test",
    ),
    (
        "an executable not named like a nexus binary is never refused",
        {"binaries": {}, "paths": {}},
        [exe("some-tool"), Target(name="lib-test", kind="STATIC_LIBRARY", artifact=None)],
        None,
    ),
]


def main() -> int:
    failures = 0
    for title, manifest, targets, expected in CASES:
        got = refused(manifest, targets)
        ok = got is None if expected is None else (got is not None and got.startswith(f"error: {expected} "))
        if not ok:
            failures += 1
            print(f"FAIL {title}: expected {'no refusal' if expected is None else 'a refusal of ' + expected}, got {got!r}")
        elif VERBOSE:
            print(f"ok   {title}")
    print(f"targets-self-test: {len(CASES) - failures} of {len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
