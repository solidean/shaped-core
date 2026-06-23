"""Log-file naming, capture reporting, and JUnit XML handling.

These helpers keep dev.py quiet-by-default: each subprocess writes its streams
to per-step files under build/<preset>/run-logs/, and we only print compact
pointers to those files plus a pass/fail summary.
"""

from __future__ import annotations

import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from .models import StepResult, TestSummary


def write_sidecar(build_dir: Path, name: str, data: dict) -> Path:
    """Write a machine-readable JSON sidecar into build_dir and return its path."""
    build_dir.mkdir(parents=True, exist_ok=True)
    path = build_dir / name
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    return path


def step_fields(result: StepResult, build_dir: Path) -> dict:
    """Common sidecar fields describing a single step, with logs relative to build_dir."""
    def rel(p: Path) -> str:
        try:
            return str(p.relative_to(build_dir))
        except ValueError:
            return str(p)

    return {
        "step_type": result.step_type,
        "name": result.name,
        "command": result.command,
        "returncode": result.returncode,
        "duration_s": round(result.duration_s, 3),
        "timed_out": result.timed_out,
        "stdout_log": rel(result.stdout_log),
        "stderr_log": rel(result.stderr_log),
    }


def _slug(label: str) -> str:
    """Sanitize a step label for use in a log file name."""
    return re.sub(r"[^A-Za-z0-9._-]+", "-", label)


def step_log_paths(build_dir: Path, step_type: str, name: str | None) -> tuple[Path, Path]:
    """Return (stdout_path, stderr_path) for a step, creating the run-logs dir.

    The file stem is the logical step name (`run-log-<name>`), falling back to
    `step_type` when a step has no specific name (e.g. configure). Names are
    distinct per step within a preset's build dir, so no numeric prefix is
    needed to keep them apart.
    """
    log_dir = build_dir / "run-logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    stem = f"run-log-{_slug(name or step_type)}"
    return log_dir / f"{stem}.stdout.txt", log_dir / f"{stem}.stderr.txt"


_NINJA_EDGE_RE = re.compile(r"^\[\d+/\d+\]")


def ninja_built_count(stdout_log: Path) -> int:
    """Count the build edges ninja executed, from a captured build stdout.

    Ninja prints one `[done/total] <action>` line per edge it runs, so the
    number of such lines is how many files/actions were (re)built. Returns 0
    when nothing was rebuilt ("ninja: no work to do.") or the log is missing.
    """
    try:
        text = stdout_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    return sum(1 for line in text.splitlines() if _NINJA_EDGE_RE.match(line))


def report_capture(path: Path) -> None:
    """Print 'path  [X lines, Y kB]' for a capture, skipping empty files."""
    try:
        data = path.read_bytes()
    except OSError:
        return
    if not data:
        return
    lines = len(data.splitlines())
    kb = len(data) / 1024
    print(f"  -> {path}  [{lines} lines, {kb:.1f} kB]", file=sys.stderr)


_BRACKET_LOG_RE = re.compile(r"^\[[^\]]*\]\[[^\]]*\]")


def strip_log_lines(text: str) -> str:
    """Drop '[timestamp][severity] ...' lines some libs print to stdout, so the
    remaining text parses cleanly as JSON (used by --list-json)."""
    return "\n".join(
        line for line in text.splitlines() if not _BRACKET_LOG_RE.match(line)
    )


def write_step_junit(
    path: Path,
    *,
    name: str,
    result: StepResult,
    tail_lines: int = 400,
) -> TestSummary:
    """Synthesize a per-binary JUnit XML from a captured run.

    The test runner here does not emit JUnit itself, so we model each binary as a
    single test case: failed when the process returned non-zero, with the tail of
    its captured stderr embedded as the failure body. This keeps a machine-
    readable result around for `test_diag` and is framework-agnostic.
    """
    failed = result.returncode != 0
    failures = 1 if failed else 0

    message = ""
    if failed:
        try:
            text = result.stderr_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        if not text.strip():
            try:
                text = result.stdout_log.read_text(encoding="utf-8", errors="replace")
            except OSError:
                text = ""
        message = "\n".join(text.splitlines()[-tail_lines:])

    suites = ET.Element("testsuites")
    suites.set("name", name)
    suites.set("tests", "1")
    suites.set("failures", str(failures))
    suites.set("errors", "0")
    suites.set("skipped", "0")
    suites.set("time", f"{result.duration_s:.5f}")

    suite = ET.SubElement(suites, "testsuite")
    suite.set("name", name)
    suite.set("tests", "1")
    suite.set("failures", str(failures))
    suite.set("errors", "0")
    suite.set("skipped", "0")
    suite.set("time", f"{result.duration_s:.5f}")

    case = ET.SubElement(suite, "testcase")
    case.set("classname", name)
    case.set("name", name)
    case.set("time", f"{result.duration_s:.5f}")
    if failed:
        failure = ET.SubElement(case, "failure")
        reason = "timed out" if result.timed_out else f"exit code {result.returncode}"
        failure.set("message", reason)
        failure.text = message

    path.parent.mkdir(parents=True, exist_ok=True)
    tree = ET.ElementTree(suites)
    ET.indent(tree, space="  ")
    tree.write(str(path), encoding="unicode", xml_declaration=True)

    return TestSummary(
        binary=name, tests=1, failures=failures, errors=0, skipped=0, time_s=result.duration_s, assertions=0
    )


def parse_junit(path: Path) -> TestSummary | None:
    """Parse a JUnit XML file into a TestSummary, or None if missing/unparseable."""
    if not path.is_file():
        return None
    try:
        tree = ET.parse(path)
    except ET.ParseError:
        return None
    totals = dict(tests=0, failures=0, errors=0, skipped=0, assertions=0)
    time_s = 0.0
    for suite in tree.getroot().iter("testsuite"):
        for attr in totals:
            totals[attr] += int(suite.get(attr, "0"))
        time_s += float(suite.get("time", "0"))
    return TestSummary(
        binary=path.name,
        tests=totals["tests"],
        failures=totals["failures"],
        errors=totals["errors"],
        skipped=totals["skipped"],
        time_s=time_s,
        assertions=totals["assertions"],
    )


def merge_junit(xml_paths: list[Path], output: Path) -> TestSummary:
    """Merge several JUnit XML files into one report at `output`."""
    merged = ET.Element("testsuites")
    merged.set("name", "All Tests")

    totals = dict(tests=0, failures=0, errors=0, skipped=0, assertions=0)
    time_s = 0.0
    for path in xml_paths:
        if not path.is_file():
            continue
        tree = ET.parse(path)
        for suite in tree.getroot().iter("testsuite"):
            merged.append(suite)
            for attr in totals:
                totals[attr] += int(suite.get(attr, "0"))
            time_s += float(suite.get("time", "0"))

    for attr in totals:
        merged.set(attr, str(totals[attr]))
    merged.set("time", f"{time_s:.5f}")

    output.parent.mkdir(parents=True, exist_ok=True)
    tree = ET.ElementTree(merged)
    ET.indent(tree, space="  ")
    tree.write(str(output), encoding="unicode", xml_declaration=True)
    print(f"XML report written to {output}", file=sys.stderr)
    print(
        f"  Total tests: {totals['tests']}, failures: {totals['failures']}, "
        f"errors: {totals['errors']}",
        file=sys.stderr,
    )
    return TestSummary(
        binary=output.name,
        tests=totals["tests"],
        failures=totals["failures"],
        errors=totals["errors"],
        skipped=totals["skipped"],
        time_s=time_s,
        assertions=totals["assertions"],
    )
