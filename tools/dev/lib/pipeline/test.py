"""Test: run test binaries and capture results.

Framework-agnostic: each test executable runs through the standard step machinery, and pass/fail is its exit code.
A positional argument is passed through to the binary as a test-name filter, a convention most runners honor.

A binary that writes no JUnit report of its own gets a synthesized single-case sidecar (see logs.write_step_junit), so test_diag and CI always have something machine-readable to parse.

With the optional build server opted in (build_server.py), the binaries run through it instead, which serves a stored pass when nothing a binary executes or reads changed.
dev.py still files what each run left (crash reports, resolved sites, kept failure logs, run logs), so a served run reads like one dev.py started.

Public API:
    test(presets, binary_names, ...) -> list[dict]   (per-binary run records)
"""

from __future__ import annotations

import functools
import itertools
import json
import os
import shutil
import time
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

from ..core import console, crash_reports, profile, ui
from ..core.logs import capture_line, parse_junit, step_fields, step_log_paths, write_sidecar, write_step_junit
from ..core.models import Preset, StepResult, Target, TestSummary
from ..core.process import describe_exit_code, emsdk_env, run_step
from ..toolchain import monado
from ..toolchain import jsruntime as jsr
from ..project import targets as targets_mod
from . import build_server, sites
from .eligibility import nexus_filter

# nexus prints this when a name filter matches no tests in a binary.
# With a filter active that is "nothing to run here", not a failure.
_NO_TESTS_SENTINEL = "did not select any tests"


def _selected_no_tests(stderr_log: Path) -> bool:
    try:
        return _NO_TESTS_SENTINEL in stderr_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


def _lsan_suppression_env() -> dict[str, str]:
    """LSAN_OPTIONS pointing at the runtime suppression list.

    Harmless on a non-sanitized build, which ignores the variable, so this is set unconditionally rather than probed
    per preset.
    Appends to whatever the caller already set, so a developer debugging a leak can add options without losing these.
    """
    suppressions = Path(__file__).resolve().parents[3] / "cmake" / "lsan-suppressions.txt"
    if not suppressions.is_file():
        return {}
    existing = os.environ.get("LSAN_OPTIONS", "")
    value = f"suppressions={suppressions.as_posix()}"
    return {"LSAN_OPTIONS": f"{existing}:{value}" if existing else value}


def _tsan_suppression_env() -> dict[str, str]:
    """TSAN_OPTIONS pointing at the runtime suppression list.

    Set unconditionally for the same reason as the LSan one: a build without ThreadSanitizer ignores the variable.
    Appends to whatever the caller already set, so a developer chasing one race can add options without losing these.
    """
    suppressions = Path(__file__).resolve().parents[3] / "cmake" / "tsan-suppressions.txt"
    if not suppressions.is_file():
        return {}
    existing = os.environ.get("TSAN_OPTIONS", "")
    value = f"suppressions={suppressions.as_posix()}"
    return {"TSAN_OPTIONS": f"{existing}:{value}" if existing else value}


def _sanitizer_path_env(build_dir: Path) -> dict[str, str]:
    """PATH override so a Windows ASan binary finds its dynamic runtime DLL.

    clang-cl links the ASan runtime dynamically, so the instrumented test exe needs clang_rt.asan_dynamic-*.dll at launch.
    configure records the runtime directory in the cache as SC_ASAN_RUNTIME_DIR, and prepending it to PATH resolves the DLL without copying it next to each binary.
    Empty for any build that did not record it, which is the common case.
    """
    cache = build_dir / "CMakeCache.txt"
    rtdir = ""
    try:
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("SC_ASAN_RUNTIME_DIR:"):
                rtdir = line.partition("=")[2].strip()
                break
    except OSError:
        return {}
    if not rtdir:
        return {}
    existing = os.environ.get("PATH", "")
    return {"PATH": rtdir + os.pathsep + existing if existing else rtdir}


def _preset_env(preset: Preset) -> dict[str, str]:
    """Per-preset additions every binary gets, such as the Windows ASan runtime dir on PATH."""
    env = _sanitizer_path_env(preset.build_dir)
    env.update(_lsan_suppression_env())
    env.update(_tsan_suppression_env())
    env.update(monado.test_env())
    return env


def server_env(preset: Preset, root: Path, base: dict[str, str] | None = None,
               preset_env: dict[str, str] | None = None) -> dict[str, str]:
    """The environment a preset's binaries run in through the build server, which keys its results on it.

    `ship`'s proof sends the same, so what it looks up is what `test` through the server stored.
    """
    return {
        **os.environ, **(base or {}), **(_preset_env(preset) if preset_env is None else preset_env),
        "SC_SOURCE_ROOT": str(root), "NX_CRASH_DIR": str(crash_reports.incoming_dir(preset.build_dir)),
        "NX_LOG_REC_DIR": str(log_rec_dir(preset)),
    }


def log_rec_dir(preset: Preset) -> Path:
    """Where every nexus binary keeps its compacted recording of the run, `<binary>.ccrec`, beside the run's logs."""
    return preset.build_dir / "run-logs"


def _harvest_test_timings(timings_path: Path, *, binary: str, preset: str) -> None:
    """Add one job per test from the timings sidecar nexus wrote under --timings-json.

    A test that dispatched children through nx::invoke_tests is a `testcase-driver` container rather than a `testcase` leaf.
    Its slice encloses its children's, which are entries of their own, so summing both would count that time twice.

    Silently adds nothing when the file is missing or unreadable — a crash before the report, or a runner that is not nexus.
    The step itself is still in the profile, so the run's time stays accounted for.
    """
    try:
        doc = json.loads(timings_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return
    jobs = []
    for t in doc.get("tests", []):
        if "name" not in t or "start" not in t or "end" not in t:
            continue
        children = int(t.get("children", 0))
        extra = {"binary": binary, "preset": preset, "thread": t.get("thread"), "failed": t.get("failed", False)}
        if children > 0:
            extra["children"] = children
        # What serializes the test, so a slice in the trace says why nothing ran beside it.
        for key in ("exclusive", "main_thread", "tags", "phase"):
            if key in t:
                extra[key] = t[key]
        jobs.append(profile.Job(
            name=t["name"], type="testcase-driver" if children > 0 else "testcase",
            start=float(t["start"]), end=float(t["end"]), extra=extra, container=children > 0,
        ))
    profile.add_jobs(jobs)


def _test_extra(xml_path: Path) -> str:
    """Summary suffix for a test step: test cases run and checks evaluated.

    Reads the JUnit XML the binary just wrote, and is empty when there is none — --no-xml, or a crash or timeout before the report was flushed.
    """
    summary = parse_junit(xml_path)
    if summary is None:
        return ""
    return f" ({summary.tests} tests, {summary.assertions} checks)"


def _binary_timeout(target: Target, timeout: float | None, explicit: bool) -> float | None:
    """The timeout one binary runs under: the default, raised to what the binary declared unless the caller was explicit."""
    if explicit or not timeout or target.timeout_secs is None:
        return timeout
    return max(timeout, target.timeout_secs)


def _echo_recording(recording: Path) -> None:
    """Say where --rec kept a binary's recording, and how large it is."""
    if recording.is_file():
        ui.write_line(f"  recording kept: {recording} ({recording.stat().st_size / (1 << 20):.1f} MiB)")
    else:
        ui.write_line(console.yellow("  no recording: the run wrote none (--no-recording, or a crash before the end)"))


def _echo_sample_report(stdout_log: Path | None, recording: Path) -> None:
    """Print the hot-function report nexus --sample wrote at the end of its stdout, and where the recording went."""
    text = stdout_log.read_text(encoding="utf-8", errors="replace") if stdout_log and stdout_log.is_file() else ""
    marker = "where the busy time went, sampled:"
    at = text.rfind(marker)
    if at < 0:
        ui.write_line(console.yellow("  no sample report: the run wrote none (--no-recording, or a crash before the end)"))
        return
    for line in text[at:].splitlines():
        if not line.strip():
            break
        ui.write_line(f"  {line}")
    if recording.is_file():
        ui.write_line(console.dim(f"  recording: {recording}"))


def _resolve_sites(stdout_log: Path | None, stderr_log: Path | None, artifact: Path, build_dir: Path, root: Path) -> None:
    """Print a failed run's opaque sites resolved, and append them to its stderr log; never raises."""
    try:
        lines = sites.report([stdout_log, stderr_log], artifact, build_dir, root)
        if lines and stderr_log is not None:
            with open(stderr_log, "a", encoding="utf-8") as f:
                f.write("\n[dev.py] the sites above, resolved through the build's debug info:\n" + "\n".join(lines) + "\n")
    except Exception as e:  # a report on a failure must not become a failure of its own
        ui.write_line(console.yellow(f"  sites not resolved: {e}"))


def _junit_fields(summary: TestSummary | None) -> dict | None:
    if summary is None:
        return None
    return {
        "tests": summary.tests,
        "failures": summary.failures,
        "errors": summary.errors,
        "skipped": summary.skipped,
        "assertions": summary.assertions,
        "time_s": round(summary.time_s, 3),
        "cpu_load": summary.cpu_load,
        "cores_used": summary.cores_used,
        "peak_resident_bytes": summary.peak_resident_bytes,
        "serial_s": summary.serial_s,
        "serial_group": summary.serial_group,
        "serial_group_s": summary.serial_group_s,
        "skip_reasons": [list(s) for s in summary.skip_reasons],
    }


def _record(result: StepResult, target: Target, preset: Preset, *, root: Path, test_name: str | None, write_xml: bool,
            verbose: bool) -> dict | None:
    """Files what one finished run of `target` left (crash report, resolved sites, kept logs, JUnit) and returns its record.

    None when a name filter selected none of its tests, which is not a run to count.
    Shared by the runs dev.py starts itself and the ones the build server reports, so both leave the same files behind.
    """
    name = target.name
    xml_path = target.artifact.parent / f"{target.artifact.name}.results.xml"
    kept = crash_reports.collect(preset.build_dir, target.artifact, result.stdout_log, result.stderr_log)
    if kept is not None:
        ui.write_line(console.yellow(f"  crash report kept: {kept}"))

    # With a name filter, "no matching tests in this binary" isn't a failure.
    if test_name and not result.ok and _selected_no_tests(result.stderr_log):
        if verbose:
            ui.write_line(console.dim(f"  {name}: no tests match {test_name!r}, skipping"))
        return None

    # Below SC_SOURCE_SITES=FULL a failure names its sites by address.
    # Resolved here and appended to the stderr log, so the kept logs and test_diag carry them too.
    if not result.ok and not preset.is_cross_compiling:
        _resolve_sites(result.stdout_log, result.stderr_log, target.artifact, preset.build_dir, root)

    # a failure that left no crash report keeps its logs too: the next run overwrites them
    if not result.ok and kept is None:
        failed = crash_reports.keep_failure(preset.build_dir, target.artifact,
                                            [result.stdout_log, result.stderr_log, xml_path if write_xml else None])
        ui.write_line(console.yellow(f"  failed run's logs kept: {failed}"))

    summary = None
    if write_xml:
        # Prefer the binary's own native report, one case per nexus test.
        # Fall back to a synthesized single-case sidecar when it wrote nothing.
        native = None
        if xml_path.is_file() and xml_path.stat().st_size > 0:
            native = parse_junit(xml_path)
        summary = native or write_step_junit(xml_path, name=name, result=result)

    return {
        "name": name,
        # Which preset ran it.
        # The sidecar is per build dir so it is implied there, but the records a caller gets back span every
        # preset in one list, and attributing a slow run needs it.
        "preset": preset.name,
        "artifact": str(target.artifact),
        "junit": _junit_fields(summary),
        **step_fields(result, preset.build_dir),
    }


def _server_bypass(*, sample: bool, rec: bool, write_xml: bool, extra_env: bool, preset: Preset) -> str | None:
    """Why this preset's binaries run without the build server although it is opted in; None when they can go through it.

    The server stores and restores a binary's console output and JUnit report only, so a run writing anything else runs the plain way.
    """
    if preset.is_emscripten or preset.is_cross_compiling:
        return "a cross-compiled preset"
    if sample:
        return "--sample"
    if rec:
        return "--rec"
    if profile.enabled():
        return "--profile (per-test timings)"
    if not write_xml:
        return "--no-xml-reports"
    if extra_env:
        return "per-binary environment"
    return None


def _split_jobs(args: list[str]) -> tuple[list[str], int | None]:
    """`--jobs N` out of the runner's arguments, since the server sizes each binary's width itself (--threads); the last one wins."""
    rest: list[str] = []
    jobs = None
    it = iter(args)
    for a in it:
        if a == "--jobs":
            value = next(it, "")
            jobs = int(value) if value.isdigit() else jobs
        else:
            rest.append(a)
    return rest, jobs


def _served_result(r: dict, target: Target, preset: Preset, command: list[str], timeout: float | None) -> StepResult:
    """A StepResult for one binary the build server ran or served, with its output in dev.py's own run logs."""
    stdout_path, stderr_path = step_log_paths(preset.build_dir, "test", target.name)
    log = Path(r["log"]) if r.get("log") else None
    try:
        if log is not None and log.is_file():
            shutil.copyfile(log, stdout_path)
        else:
            stdout_path.write_text("", encoding="utf-8")
    except OSError as e:
        stdout_path.write_text(f"[dev.py] the build server's log {log} could not be read: {e}\n", encoding="utf-8")
    status = r["status"]
    note = {
        "TimedOut": f"[dev.py] TIMEOUT: '{target.name}' exceeded {timeout or 0:.0f}s, then the build server killed it.\n",
        "Missing": f"[dev.py] {target.artifact} is not built.\n",
        "Cancelled": "[dev.py] cancelled.\n",
    }.get(status, "")
    stderr_path.write_text(note, encoding="utf-8")
    code = r.get("exit_code")
    returncode = {"Passed": 0, "NoTests": 0, "TimedOut": 124, "Missing": 127, "Cancelled": 130}.get(
        status, code if code not in (None, 0) else 1)
    return StepResult("test", target.name, command, returncode, (r.get("ms") or 0) / 1000, stdout_path, stderr_path,
                      timed_out=status == "TimedOut")


# How much of a failed binary's output the summary repeats; the whole of it is in the run log.
_FAILED_TAIL_LINES = 30


def _served_summary(r: dict, result: StepResult, xml_path: Path) -> None:
    """Prints one binary's line as run_step would, marked with whether the server served a stored pass or ran it."""
    name = result.name
    ms = (r.get("ms") or 0)
    if result.ok:
        if r.get("cached"):
            how = console.dim(f", cached (a stored pass of {(r.get('stored_ms') or 0) / 1000:.1f} s)")
        else:
            # `why` is the server's reason it could not serve a stored pass, such as which input changed
            why = ", verifying a stored pass" if r.get("verified") is not None else f" ({r['why']})" if r.get("why") else ""
            how = console.dim(f", ran{why}")
        ui.write_line(console.green(f"  {name} succeeded{_test_extra(xml_path)} in {ms:.0f} ms") + how)
        return
    try:
        lines = result.stdout_log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        lines = []
    for line in lines[-_FAILED_TAIL_LINES:]:
        ui.write_line(console.dim(f"  | {line}"))
    for path in (result.stdout_log, result.stderr_log):
        pointer = capture_line(path)
        if pointer:
            ui.write_line(pointer)
    if result.timed_out:
        verdict = f"TIMED OUT (killed) in {ms:.0f} ms"
    elif r["status"] == "Missing":
        verdict = "is not built"
    else:
        verdict = f"failed{_test_extra(xml_path)}{describe_exit_code(result.returncode)} in {ms:.0f} ms"
    seed = f", seed {r['seed']}" if r.get("seed") is not None else ""
    ui.write_line(console.red(f"  {name} {verdict}{seed}"))


# Numbers each request to the build server, so a report can tell which binaries shared one wall clock (report.wall_s).
_REQUESTS = itertools.count()


def _test_through_server(
    server: build_server.Session,
    preset: Preset,
    targets: list[Target],
    *,
    root: Path,
    test_name: str | None,
    extra_args: list[str],
    env: dict[str, str],
    timeout: float | None,
    timeout_is_explicit: bool,
    watchdog_fraction: float | None,
    rerun: bool,
    mirror: bool,
    verbose: bool,
) -> tuple[list[dict], list[Target]]:
    """Runs `targets` through the build server: the records of those it ran or served, and the targets left to run the plain way.

    One request per distinct timeout, since a request's timeout and arguments hold for each of its binaries, the watchdog among them.
    """
    runner_args, jobs = _split_jobs(extra_args)
    groups: dict[float | None, list[Target]] = {}
    for t in targets:
        groups.setdefault(_binary_timeout(t, timeout, timeout_is_explicit) or None, []).append(t)

    records: list[dict] = []
    left: list[Target] = []
    for binary_timeout, group in groups.items():
        # The arguments as the plain run builds them, but for the --junit-xml and --jobs the server adds itself.
        args = [nexus_filter(test_name)] if test_name else []
        if verbose:
            args.append("-v")
        if watchdog_fraction is not None and binary_timeout:
            args += ["--watchdog", f"{binary_timeout * watchdog_fraction:g}"]
        args += runner_args
        request, started = next(_REQUESTS), time.monotonic()
        results = server.test(
            preset.build_dir, [t.name for t in group], runner_args=args, timeout=binary_timeout,
            threads=jobs or None, rerun=rerun, verbose=verbose, env=env,
            run=functools.partial(run_step, step_type="test", name="build-server", build_dir=preset.build_dir,
                                  cwd=root, env=env, mirror=mirror, verbose=verbose),
        )
        if results is None:
            left.extend(group)
            continue
        request_s = time.monotonic() - started
        by_name = {r["name"]: r for r in results}
        for t in group:
            r = by_name[t.name]
            if r["status"] == "NoTests":
                if verbose:
                    ui.write_line(console.dim(f"  {t.name}: no tests selected, skipping"))
                continue
            seed = ["--seed", str(r["seed"])] if r.get("seed") is not None else []
            command = [str(t.artifact), "--tests", *args, *seed]
            result = _served_result(r, t, preset, command, binary_timeout)
            xml_path = t.artifact.parent / f"{t.artifact.name}.results.xml"
            _served_summary(r, result, xml_path)
            record = _record(result, t, preset, root=root, test_name=test_name, write_xml=True, verbose=verbose)
            if record is not None:
                record["build_server"] = {k: r.get(k) for k in ("cached", "verified", "stored_ms", "seed")}
                # The server runs a request's binaries side by side, so their own times overlap; the request's is the wall time.
                record["build_server"].update(request=request, request_s=request_s)
                records.append(record)

    served = [r for r in records if r["build_server"]["cached"]]
    if records:
        saved = sum(r["build_server"]["stored_ms"] or 0 for r in served) / 1000
        ui.write_line(console.dim(f"  build server: of {len(records)} test binaries {len(records) - len(served)} ran, "
                                  f"{len(served)} served from its cache ({saved:.1f} s of runs saved)"))
    return records, left


def test(
    presets: list[Preset],
    binary_names: list[str],
    *,
    root: Path,
    test_name: str | None = None,
    extra_args: list[str] | None = None,
    env: dict[str, str] | None = None,
    extra_env_for: Callable[[str], dict[str, str]] | None = None,
    timeout: float | None = None,
    timeout_is_explicit: bool = False,
    watchdog_fraction: float | None = None,
    write_xml: bool = True,
    mirror: bool = False,
    verbose: bool = False,
    emsdk_path: str | None = None,
    runtime: jsr.JsRuntimeRequest | None = None,
    sample: bool = False,
    rec: bool = False,
    server: build_server.Session | None = None,
    rerun: bool = False,
) -> list[dict]:
    """Run the named test binaries, optionally filtered by `test_name`.

    `binary_names` are already-filtered target names, so the caller decides which executables are tests.
    For each preset they are resolved to that preset's built artifacts.
    With `test_name` set, a binary that reports no matching tests is skipped rather than counted as a failure.
    Each binary gets a JUnit XML next to it unless `write_xml` is False, plus one test.json sidecar per preset.
    Returns one record per executed binary.

    `timeout` is the default a binary's declared timeout (sc_nexus_binary TIMEOUT) raises, unless `timeout_is_explicit`,
    in which case it holds for every binary; None or 0 runs without one.
    `watchdog_fraction` passes each binary a `--watchdog` of that fraction of its own timeout, so the hang report still
    lands before the kill however long a binary was given.

    `sample` has each binary sample its threads for the whole run (nexus --sample), keep the recording as
    `<artifact>.ccrec` beside it, and print where the busy time went, which this echoes from the binary's stdout log.

    `extra_env_for(name)` injects per-binary environment variables, merged on top of the inherited process env and `env` rather than replacing them.
    The coverage runner uses it to point each binary's LLVM_PROFILE_FILE at a distinct file; None leaves the child env untouched.

    `server` runs the binaries through the optional build server when it serves and nothing in the run needs the plain way
    (_server_bypass); `rerun` has it run every binary even when it holds a stored pass.
    """
    extra_args = list(extra_args or [])
    all_records: list[dict] = []

    for preset in presets:
        by_name = {
            t.name: t
            for t in targets_mod.discover_targets(preset.build_dir, preset.build_type)
        }
        preset_env = _preset_env(preset)

        # Emscripten test artifacts are .js/.wasm that run under node, which the emsdk environment puts on PATH.
        # Native presets keep the inherited environment.
        preset_base_env = env
        if preset.is_emscripten:
            wasm_env = emsdk_env(emsdk_path)
            if wasm_env is None:
                ui.write_line(
                    console.yellow(
                        f"WARNING: emsdk not found for preset {preset.name!r}; "
                        f"running with the inherited environment (node may be missing). "
                        f"Pass --emsdk-path or activate emsdk."
                    )
                )
            else:
                preset_base_env = wasm_env

        # Resolved against the emsdk overlay above, since that is where node is expected to live.
        launcher_for_preset = jsr.LazyLauncher(runtime, preset_base_env)

        records: list[dict] = []
        plain_names = binary_names
        if server is not None:
            bypass = _server_bypass(sample=sample, rec=rec, write_xml=write_xml, extra_env=extra_env_for is not None,
                                    preset=preset)
            if bypass is not None:
                ui.write_line(console.dim(f"  tests run without the build server: {bypass}"))
            else:
                built = [t for t in (by_name.get(n) for n in binary_names) if t is not None and t.artifact is not None]
                # an artifact a runtime launches (a .js) is not a binary the server can start
                targets = [t for t in built if not jsr.needs_launcher(False, t.artifact)]
                served_env = server_env(preset, root, preset_base_env, preset_env)
                served, left = _test_through_server(
                    server, preset, targets, root=root, test_name=test_name, extra_args=extra_args, env=served_env,
                    timeout=timeout, timeout_is_explicit=timeout_is_explicit, watchdog_fraction=watchdog_fraction,
                    rerun=rerun, mirror=mirror, verbose=verbose,
                )
                records += served
                all_records += served
                plain_names = [t.name for t in built if t not in targets or t in left]

        with ui.phase(f"test {preset.name}", total=len(plain_names)) as binaries:
            for name in plain_names:
                binaries.advance(name)
                target = by_name.get(name)
                if target is None or target.artifact is None:
                    continue
                xml_path = target.artifact.parent / f"{target.artifact.name}.results.xml"

                # Emscripten emits a non-executable .js/.wasm artifact; hand it to the JS runtime instead.
                # A missing runtime is a setup problem rather than a bug, so it is reported as one line rather than as a
                # traceback out of the middle of a test run.
                try:
                    launcher = (launcher_for_preset.prefix()
                                if jsr.needs_launcher(preset.is_emscripten, target.artifact) else [])
                except jsr.NotFound as e:
                    raise SystemExit(f"error: {e}") from None
                # --tests first: a nexus binary runs its default app or command, or lists what it holds, unless the line
                # selects tests — and ahead of the name, so a test named like one of its commands still means the test.
                cmd = [*launcher, str(target.artifact), "--tests"]
                if test_name:
                    cmd.append(nexus_filter(test_name))
                # Forward verbosity to the runner: nexus' -v prints "- start <test>" before each test, so a crash or hang pinpoints the last one that started.
                # A harmless positional for other runners.
                if verbose:
                    cmd.append("-v")
                # nexus writes a native per-test JUnit report here, and a non-nexus or crashed binary simply will not — synthesis below covers that.
                # Clear any stale report first, so a crashed run cannot be read as fresh.
                if write_xml:
                    xml_path.unlink(missing_ok=True)
                    cmd += ["--junit-xml", str(xml_path)]
                # Under --profile, nexus also reports where each test sat on the timeline, which becomes one trace slice per test.
                timings_path = target.artifact.parent / f"{target.artifact.name}.timings.json"
                if profile.enabled():
                    timings_path.unlink(missing_ok=True)
                    cmd += ["--timings-json", str(timings_path)]
                binary_timeout = _binary_timeout(target, timeout, timeout_is_explicit)
                # Before extra_args, so a --watchdog the caller passed through to the runner still wins.
                if watchdog_fraction is not None and binary_timeout:
                    cmd += ["--watchdog", f"{binary_timeout * watchdog_fraction:g}"]
                sample_path = target.artifact.parent / f"{target.artifact.name}.ccrec"
                if sample:
                    sample_path.unlink(missing_ok=True)
                    cmd += ["--sample", str(sample_path)]
                elif rec:
                    sample_path.unlink(missing_ok=True)
                    cmd += ["--rec", str(sample_path)]
                cmd += extra_args

                # Per-binary and per-preset env layer onto the inherited environment, so PATH and the MSVC vars the child needs are never dropped.
                run_env = preset_base_env
                layered = {**preset_env, **(extra_env_for(name) if extra_env_for else {})}
                # Where the binary finds the source tree's data (nexus/source_root.hh).
                # Named, so a run never depends on the walk up to the build's CMakeCache.txt, nor inherits another checkout's.
                layered["SC_SOURCE_ROOT"] = str(root)
                # A native nexus binary keeps every crash or hang report in files too, which collect() files below.
                if not preset.is_cross_compiling:
                    layered.setdefault("NX_CRASH_DIR", str(crash_reports.incoming_dir(preset.build_dir)))
                    layered.setdefault("NX_LOG_REC_DIR", str(log_rec_dir(preset)))
                if layered:
                    run_env = {**os.environ, **(preset_base_env or {}), **layered}

                result = run_step(
                    cmd,
                    step_type="test",
                    name=name,
                    build_dir=preset.build_dir,
                    cwd=root,
                    env=run_env,
                    timeout=binary_timeout or None,
                    mirror=mirror,
                    verbose=verbose,
                    summary_extra=(lambda r, xp=xml_path: _test_extra(xp)) if write_xml else None,
                )

                if profile.enabled():
                    _harvest_test_timings(timings_path, binary=name, preset=preset.name)

                if sample:
                    _echo_sample_report(result.stdout_log, sample_path)
                elif rec:
                    _echo_recording(sample_path)

                record = _record(result, target, preset, root=root, test_name=test_name, write_xml=write_xml,
                                 verbose=verbose)
                if record is not None:
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
