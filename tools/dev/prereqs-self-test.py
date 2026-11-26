#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for a project's own prerequisites — `Prerequisite` and its registry in tools/dev/lib/pipeline/prereqs.py.

A prerequisite fails quietly: one whose fingerprint never reaches the configure fingerprint is never run again after the first configure.
Each case registers its own prerequisites over a throwaway tree, so the cases run one after the other.

    uv run tools/dev/prereqs-self-test.py [-v]
"""

from __future__ import annotations

import sys
import tempfile
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.pipeline import fingerprint, prereqs  # noqa: E402
from tools.dev.lib.pipeline.prereqs import Prerequisite  # noqa: E402

VERBOSE = "-v" in sys.argv


def make_tree(root: Path) -> Path:
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.28)\n", encoding="utf-8")
    (root / "build").mkdir()
    return root / "build"


def test_no_prerequisites_leave_the_fingerprint_as_it_was(root: Path) -> None:
    make_tree(root)
    prereqs.register_project_prerequisites(())
    assert prereqs.project_fingerprint(root) == ""
    before = fingerprint.compute(root)
    assert fingerprint.compute(root) == before


def test_a_changed_input_makes_configure_stale(root: Path) -> None:
    build = make_tree(root)
    state = {"zip": "release-1"}
    prereqs.register_project_prerequisites((Prerequisite("sdk", ensure=lambda r, p: None, fingerprint=lambda r: state["zip"]),))
    fingerprint.save(build, root)
    assert fingerprint.is_current(build, root), "an unchanged input must keep configure current"
    state["zip"] = "release-2"
    assert not fingerprint.is_current(build, root), "a changed input must make configure stale"


def test_registering_a_prerequisite_makes_configure_stale(root: Path) -> None:
    build = make_tree(root)
    prereqs.register_project_prerequisites(())
    fingerprint.save(build, root)
    prereqs.register_project_prerequisites((Prerequisite("sdk", ensure=lambda r, p: None),))
    assert not fingerprint.is_current(build, root), "a build configured before the prerequisite existed must configure again"


def test_ensure_runs_each_in_registration_order(root: Path) -> None:
    calls: list[tuple[str, Path, str]] = []
    prereqs.register_project_prerequisites((
        Prerequisite("first", ensure=lambda r, p: calls.append(("first", r, p))),
        Prerequisite("second", ensure=lambda r, p: calls.append(("second", r, p))),
    ))
    prereqs.ensure_project(root, "relwithdebinfo-clang")
    assert calls == [("first", root, "relwithdebinfo-clang"), ("second", root, "relwithdebinfo-clang")], calls


def run_case(t) -> str | None:
    try:
        with tempfile.TemporaryDirectory() as tmp:
            t(Path(tmp).resolve())
            return None
    except Exception as e:  # noqa: BLE001 — a self-test reports every failure rather than stopping at the first
        return f"{type(e).__name__}: {e}" + (f"\n{traceback.format_exc()}" if VERBOSE else "")
    finally:
        prereqs.register_project_prerequisites(())


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        error = run_case(t)
        if error is not None:
            failed += 1
            print(f"FAIL {t.__name__}: {error}")
        elif VERBOSE:
            print(f"ok   {t.__name__}")

    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
