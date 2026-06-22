"""Test: run test binaries and capture results.

The runner is deliberately framework-agnostic: it runs each test executable,
captures its streams via the standard step machinery, and judges pass/fail by
exit code. A positional argument is passed through to the binary as a test-name
filter — a convention most runners (including the in-repo nexus) honor.

Because the test binaries do not emit JUnit themselves, we synthesize a JUnit
sidecar per binary so downstream tooling (e.g. test_diag) and CI have a
machine-readable result to parse.

Public API:
    test(presets, binary_names, ...) -> list[dict]   (per-binary run records)
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from . import targets as targets_mod
from .logs import step_fields, write_sidecar, write_step_junit
from .models import Preset
from .process import run_step

# nexus prints this when a name filter matches no tests in a binary; with a
# filter active we treat that as "nothing to run here", not a failure.
_NO_TESTS_SENTINEL = "did not select any tests"


def _selected_no_tests(stderr_log: Path) -> bool:
    try:
        return _NO_TESTS_SENTINEL in stderr_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def test(
    presets: list[Preset],
    binary_names: list[str],
    *,
    root: Path,
    test_name: str | None = None,
    extra_args: list[str] | None = None,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
    write_xml: bool = True,
    mirror: bool = False,
    verbose: bool = False,
) -> list[dict]:
    """Run the named test binaries, optionally filtered by `test_name`.

    `binary_names` are already-filtered target names (the caller decides which
    executables are tests). For each preset the names are resolved to that
    preset's built artifacts. When `test_name` is set, a binary that reports no
    matching tests is skipped rather than counted as a failure. Each binary gets
    a synthesized JUnit XML next to it unless `write_xml` is False; a test.json
    sidecar is written per preset. Returns one record per executed binary.
    """
    extra_args = list(extra_args or [])
    all_records: list[dict] = []

    for preset in presets:
        by_name = {
            t.name: t
            for t in targets_mod.discover_targets(preset.build_dir, preset.build_type)
        }
        records: list[dict] = []
        for name in binary_names:
            target = by_name.get(name)
            if target is None or target.artifact is None:
                continue
            cmd = [str(target.artifact)]
            if test_name:
                cmd.append(test_name)
            cmd += extra_args

            result = run_step(
                cmd,
                label=name,
                step=3,
                build_dir=preset.build_dir,
                cwd=root,
                env=env,
                timeout=timeout,
                mirror=mirror,
                verbose=verbose,
            )

            # With a name filter, "no matching tests in this binary" isn't a failure.
            if test_name and not result.ok and _selected_no_tests(result.stderr_log):
                if verbose:
                    print(f"  {name}: no tests match {test_name!r}, skipping")
                continue

            summary = None
            if write_xml:
                xml_path = target.artifact.parent / f"{target.artifact.name}.results.xml"
                summary = write_step_junit(xml_path, name=name, result=result)

            record = {
                "name": name,
                "artifact": str(target.artifact),
                "junit": (
                    {
                        "tests": summary.tests,
                        "failures": summary.failures,
                        "errors": summary.errors,
                        "skipped": summary.skipped,
                        "time_s": round(summary.time_s, 3),
                    }
                    if summary
                    else None
                ),
                **step_fields(result, preset.build_dir),
            }
            records.append(record)
            all_records.append(record)

        totals = {
            "binaries": len(records),
            "failed_binaries": sum(1 for r in records if r["returncode"] != 0),
        }
        write_sidecar(
            preset.build_dir,
            "test.json",
            {
                "timestamp": datetime.now().isoformat(timespec="seconds"),
                "test_name": test_name,
                "extra_args": extra_args,
                "binaries": records,
                "totals": totals,
            },
        )

    return all_records
