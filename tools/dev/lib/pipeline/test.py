"""Test: run test binaries and capture results.

Framework-agnostic: each test executable runs through the standard step machinery, and pass/fail is its exit code.
A positional argument is passed through to the binary as a test-name filter, a convention most runners honor.

A binary that writes no JUnit report of its own gets a synthesized single-case sidecar (see logs.write_step_junit), so test_diag and CI always have something machine-readable to parse.

Public API:
    test(presets, binary_names, ...) -> list[dict]   (per-binary run records)
"""

from __future__ import annotations

import os
import sys
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

from ..core import console
from ..core.logs import parse_junit, step_fields, write_sidecar, write_step_junit
from ..core.models import Preset
from ..core.process import emsdk_env, run_step
from ..project import targets as targets_mod

# Artifact suffixes that are not directly runnable and must be launched via node.
# Emscripten emits a `<name>.js` loader next to the `.wasm`.
_WASM_LAUNCH_SUFFIXES = {".js", ".mjs", ".wasm"}

# nexus prints this when a name filter matches no tests in a binary.
# With a filter active that is "nothing to run here", not a failure.
_NO_TESTS_SENTINEL = "did not select any tests"


def _selected_no_tests(stderr_log: Path) -> bool:
    try:
        return _NO_TESTS_SENTINEL in stderr_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


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


def _test_extra(xml_path: Path) -> str:
    """Summary suffix for a test step: test cases run and checks evaluated.

    Reads the JUnit XML the binary just wrote, and is empty when there is none — --no-xml, or a crash or timeout before the report was flushed.
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
    extra_env_for: Callable[[str], dict[str, str]] | None = None,
    timeout: float | None = None,
    write_xml: bool = True,
    mirror: bool = False,
    verbose: bool = False,
    emsdk_path: str | None = None,
) -> list[dict]:
    """Run the named test binaries, optionally filtered by `test_name`.

    `binary_names` are already-filtered target names, so the caller decides which executables are tests.
    For each preset they are resolved to that preset's built artifacts.
    With `test_name` set, a binary that reports no matching tests is skipped rather than counted as a failure.
    Each binary gets a JUnit XML next to it unless `write_xml` is False, plus one test.json sidecar per preset.
    Returns one record per executed binary.

    `extra_env_for(name)` injects per-binary environment variables, merged on top of the inherited process env and `env` rather than replacing them.
    The coverage runner uses it to point each binary's LLVM_PROFILE_FILE at a distinct file; None leaves the child env untouched.
    """
    extra_args = list(extra_args or [])
    all_records: list[dict] = []

    for preset in presets:
        by_name = {
            t.name: t
            for t in targets_mod.discover_targets(preset.build_dir, preset.build_type)
        }
        # Per-preset env additions that apply to every binary, such as the Windows ASan runtime dir on PATH.
        preset_env = _sanitizer_path_env(preset.build_dir)

        # Emscripten test artifacts are .js/.wasm that run under node, which the emsdk environment puts on PATH.
        # Native presets keep the inherited environment.
        preset_base_env = env
        if preset.is_emscripten:
            wasm_env = emsdk_env(emsdk_path)
            if wasm_env is None:
                print(
                    console.yellow(
                        f"WARNING: emsdk not found for preset {preset.name!r}; "
                        f"running with the inherited environment (node may be missing). "
                        f"Pass --emsdk-path or activate emsdk."
                    ),
                    file=sys.stderr,
                )
            else:
                preset_base_env = wasm_env

        records: list[dict] = []
        for name in binary_names:
            target = by_name.get(name)
            if target is None or target.artifact is None:
                continue
            xml_path = target.artifact.parent / f"{target.artifact.name}.results.xml"

            # Emscripten emits a non-executable .js/.wasm artifact; launch it via node.
            launcher = (
                ["node"]
                if preset.is_emscripten or target.artifact.suffix.lower() in _WASM_LAUNCH_SUFFIXES
                else []
            )
            cmd = [*launcher, str(target.artifact)]
            if test_name:
                cmd.append(test_name)
            # Forward verbosity to the runner: nexus' -v prints "- start <test>" before each test, so a crash or hang pinpoints the last one that started.
            # A harmless positional for other runners.
            if verbose:
                cmd.append("-v")
            # nexus writes a native per-test JUnit report here, and a non-nexus or crashed binary simply will not — synthesis below covers that.
            # Clear any stale report first, so a crashed run cannot be read as fresh.
            if write_xml:
                xml_path.unlink(missing_ok=True)
                cmd += ["--junit-xml", str(xml_path)]
            cmd += extra_args

            # Per-binary and per-preset env layer onto the inherited environment, so PATH and the MSVC vars the child needs are never dropped.
            run_env = preset_base_env
            layered = {**preset_env, **(extra_env_for(name) if extra_env_for else {})}
            if layered:
                run_env = {**os.environ, **(preset_base_env or {}), **layered}

            result = run_step(
                cmd,
                step_type="test",
                name=name,
                build_dir=preset.build_dir,
                cwd=root,
                env=run_env,
                timeout=timeout,
                mirror=mirror,
                verbose=verbose,
                summary_extra=(lambda r, xp=xml_path: _test_extra(xp)) if write_xml else None,
            )

            # With a name filter, "no matching tests in this binary" isn't a failure.
            if test_name and not result.ok and _selected_no_tests(result.stderr_log):
                if verbose:
                    print(console.dim(f"  {name}: no tests match {test_name!r}, skipping"))
                continue

            summary = None
            if write_xml:
                # Prefer the binary's own native report, one case per nexus test.
                # Fall back to a synthesized single-case sidecar when it wrote nothing.
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
