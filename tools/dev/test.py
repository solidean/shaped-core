"""Test: run test binaries and capture results.

The runner is deliberately framework-agnostic: it runs each test executable,
captures its streams via the standard step machinery, and judges pass/fail by
exit code. A positional argument is passed through to the binary as a test-name
filter — a convention most runners (including the in-repo nexus) honor.

The in-repo nexus runner emits a native JUnit report (one case per test) when
passed `--junit-xml <file>`; we prefer that. For any binary that writes nothing
(a non-nexus runner, or a crash/timeout before flush) we synthesize a single-
case JUnit sidecar instead, so downstream tooling (e.g. test_diag) and CI always
have a machine-readable result to parse.

Public API:
    test(presets, binary_names, ...) -> list[dict]   (per-binary run records)
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from . import targets as targets_mod
from .logs import parse_junit, step_fields, write_sidecar, write_step_junit
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


def _test_extra(xml_path: Path) -> str:
    """Summary suffix for a test step: test cases run and checks evaluated.

    Reads the JUnit XML the binary just wrote; empty when there is none yet
    (--no-xml, or a crash/timeout before the report was flushed).
    """
    summary = parse_junit(xml_path)
    if summary is None:
        return ""
    return f" ({summary.tests} tests, {summary.assertions} checks)"


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
    a JUnit XML next to it unless `write_xml` is False — the binary's own native
    report when it wrote one, otherwise a synthesized single-case sidecar; a
    test.json sidecar is written per preset. Returns one record per executed
    binary.
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
            xml_path = target.artifact.parent / f"{target.artifact.name}.results.xml"

            cmd = [str(target.artifact)]
            if test_name:
                cmd.append(test_name)
            # nexus writes a native per-test JUnit report here; non-nexus or
            # crashed binaries simply won't, and we fall back to synthesis below.
            # Clear any stale report first so a crashed run can't be read as fresh.
            if write_xml:
                xml_path.unlink(missing_ok=True)
                cmd += ["--junit-xml", str(xml_path)]
            cmd += extra_args

            result = run_step(
                cmd,
                step_type="test",
                name=name,
                build_dir=preset.build_dir,
                cwd=root,
                env=env,
                timeout=timeout,
                mirror=mirror,
                verbose=verbose,
                summary_extra=(lambda r, xp=xml_path: _test_extra(xp)) if write_xml else None,
            )

            # With a name filter, "no matching tests in this binary" isn't a failure.
            if test_name and not result.ok and _selected_no_tests(result.stderr_log):
                if verbose:
                    print(f"  {name}: no tests match {test_name!r}, skipping")
                continue

            summary = None
            if write_xml:
                # Prefer the binary's own native report (one case per nexus test);
                # fall back to synthesizing a single-case sidecar when it wrote
                # nothing (non-nexus runner, crash, or timeout before flush).
                native = None
                if xml_path.is_file() and xml_path.stat().st_size > 0:
                    native = parse_junit(xml_path)
                summary = native or write_step_junit(xml_path, name=name, result=result)

            record = {
                "name": name,
                "artifact": str(target.artifact),
                "junit": (
                    {
                        "tests": summary.tests,
                        "failures": summary.failures,
                        "errors": summary.errors,
                        "skipped": summary.skipped,
                        "assertions": summary.assertions,
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
