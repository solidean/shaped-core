"""Subprocess execution: MSVC environment setup and the quiet-by-default run_step.

`run_step` is the single choke point for every external command the tooling
runs. It captures stdout/stderr to per-step log files (optionally mirroring them
live) and returns a StepResult describing the outcome.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

from .logs import report_capture, step_log_paths
from .models import StepResult


def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


# ---------------------------------------------------------------------------
# MSVC environment setup (Windows only)
# ---------------------------------------------------------------------------

def _find_vsdevcmd() -> str | None:
    """Locate VsDevCmd.bat via vswhere or well-known paths."""
    vswhere = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    )
    if vswhere.is_file():
        result = subprocess.run(
            [str(vswhere), "-latest", "-property", "installationPath"],
            capture_output=True, text=True,
        )
        if result.returncode == 0:
            candidate = Path(result.stdout.strip()) / "Common7" / "Tools" / "VsDevCmd.bat"
            if candidate.is_file():
                return str(candidate)

    for edition in ("Community", "Professional", "Enterprise"):
        candidate = Path(
            rf"C:\Program Files\Microsoft Visual Studio\2022\{edition}\Common7\Tools\VsDevCmd.bat"
        )
        if candidate.is_file():
            return str(candidate)
    return None


def msvc_env() -> dict[str, str] | None:
    """Return an environment dict with MSVC tools on PATH, or None if not needed.

    Returns None on non-Windows, when cl.exe is already on PATH, or when
    VsDevCmd.bat cannot be found.
    """
    if platform.system() != "Windows":
        return None

    # cl.exe with no args returns 1 when present; FileNotFoundError when absent.
    try:
        if subprocess.run(["cl"], capture_output=True).returncode == 1:
            return None
    except FileNotFoundError:
        pass

    vsdevcmd = _find_vsdevcmd()
    if vsdevcmd is None:
        print("WARNING: Could not find VsDevCmd.bat. MSVC builds may fail.", file=sys.stderr)
        return None

    result = subprocess.run(
        f'cmd /c "call \"{vsdevcmd}\" -arch=x64 >nul 2>&1 && set"',
        capture_output=True, text=True, shell=True,
    )
    if result.returncode != 0:
        return None

    env: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            env[k] = v
    return env


# ---------------------------------------------------------------------------
# Step runner
# ---------------------------------------------------------------------------

def _pump(src, log_file, mirror_to) -> None:
    """Read `src` line by line, writing to `log_file` and (optionally) `mirror_to`."""
    for line in iter(src.readline, ""):
        log_file.write(line)
        if mirror_to is not None:
            mirror_to.write(line)
            mirror_to.flush()
    src.close()


def run_step(
    cmd: list[str],
    *,
    step_type: str,
    name: str | None = None,
    build_dir: Path,
    cwd: Path,
    env: dict[str, str] | None = None,
    timeout: float | None = None,
    mirror: bool = False,
    verbose: bool = False,
    summary_extra: Callable[[StepResult], str] | None = None,
) -> StepResult:
    """Run a subprocess as a named step and return a StepResult.

    `step_type` is the kind of step ("configure"/"build"/"test") and `name` the
    specific thing it acts on (a target, "all", or a test binary). Prints a
    one-line `[ts] [step_type] name` banner, captures both streams to per-step
    log files under build_dir/run-logs/ (mirrored live when `mirror`), then
    prints capture pointers and a pass/fail summary. `summary_extra`, when given,
    is called with the finished StepResult and its return value is appended to
    the summary line (e.g. build/test stats); it is skipped on timeout and any
    exception it raises is swallowed. On timeout the process is killed and the
    step reports returncode 124 (conventional timeout code).
    """
    print(f"[{_ts()}] [{step_type}]" + (f" {name}" if name else ""), file=sys.stderr)
    if verbose:
        print(f"  $ {' '.join(cmd)}", file=sys.stderr)

    stdout_path, stderr_path = step_log_paths(build_dir, step_type, name)

    start = time.perf_counter()
    timed_out = False
    with open(stdout_path, "w", encoding="utf-8", errors="replace") as out_f, \
         open(stderr_path, "w", encoding="utf-8", errors="replace") as err_f:
        proc = subprocess.Popen(
            cmd, cwd=str(cwd), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        threads = [
            threading.Thread(target=_pump, args=(proc.stdout, out_f, sys.stdout if mirror else None)),
            threading.Thread(target=_pump, args=(proc.stderr, err_f, sys.stderr if mirror else None)),
        ]
        for t in threads:
            t.start()
        try:
            proc.wait(timeout=timeout if timeout else None)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            proc.wait()
        for t in threads:
            t.join()
        if timed_out:
            err_f.write(f"\n[dev.py] TIMEOUT: '{name or step_type}' exceeded {timeout:.0f}s and was killed.\n")
    duration_s = time.perf_counter() - start

    report_capture(stdout_path)
    report_capture(stderr_path)

    returncode = 124 if timed_out else proc.returncode
    result = StepResult(
        step_type=step_type,
        name=name or step_type,
        command=cmd,
        returncode=returncode,
        duration_s=duration_s,
        stdout_log=stdout_path,
        stderr_log=stderr_path,
        timed_out=timed_out,
    )

    label = name or step_type
    if timed_out:
        print(f"  {label} TIMED OUT after {timeout:.0f}s (killed) in {duration_s * 1000:.0f} ms", file=sys.stderr)
    else:
        extra = ""
        if summary_extra is not None:
            try:
                extra = summary_extra(result) or ""
            except Exception:
                extra = ""
        verb = "succeeded" if result.ok else "failed"
        print(f"  {label} {verb}{extra} in {duration_s * 1000:.0f} ms", file=sys.stderr)

    return result
