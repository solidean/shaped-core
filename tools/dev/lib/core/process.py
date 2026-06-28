"""Subprocess execution: MSVC environment setup and the quiet-by-default run_step.

`run_step` is the single choke point for every external command the tooling
runs. It captures stdout/stderr to per-step log files (optionally mirroring them
live) and returns a StepResult describing the outcome.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import threading
import time
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

from . import console
from .logs import report_capture, step_log_paths
from .models import Preset, StepResult


def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


# ---------------------------------------------------------------------------
# MSVC environment setup (Windows only)
# ---------------------------------------------------------------------------

def _find_vsdevcmd(toolset: str | None = None) -> str | None:
    """Locate VsDevCmd.bat via vswhere or well-known paths.

    With `toolset` set (a bare version like "14.51", not a path), pick the instance whose
    VC\\Tools\\MSVC actually has that toolset (prerelease included), so a pinned --toolset
    selects the right Visual Studio. Without one, use the latest instance, then fall back to
    the known VS 2022 install paths.
    """
    if toolset is not None and not ("/" in toolset or "\\" in toolset):
        from .toolset import find_msvc_instance  # local import: avoids a module-load cycle

        inst = find_msvc_instance(toolset)
        if inst is None:
            return None
        candidate = inst / "Common7" / "Tools" / "VsDevCmd.bat"
        return str(candidate) if candidate.is_file() else None

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


def msvc_env(toolset: str | None = None) -> dict[str, str] | None:
    """Return an environment dict with MSVC tools on PATH, or None if not needed.

    Returns None on non-Windows or when VsDevCmd.bat cannot be found. With no `toolset`,
    an already-on-PATH cl.exe is taken as-is (returns None to inherit the env). When a
    `toolset` is pinned, the ambient cl is ignored: the matching Visual Studio instance is
    located and `-vcvars_ver=<toolset>` selects that exact toolset (a path-valued toolset
    is handled by the clang/gcc compiler-override path, not here).
    """
    if platform.system() != "Windows":
        return None

    if toolset is None:
        # cl.exe with no args returns 1 when present; FileNotFoundError when absent.
        try:
            if subprocess.run(["cl"], capture_output=True).returncode == 1:
                return None
        except FileNotFoundError:
            pass

    vsdevcmd = _find_vsdevcmd(toolset)
    if vsdevcmd is None:
        print(console.yellow("WARNING: Could not find VsDevCmd.bat. MSVC builds may fail."), file=sys.stderr)
        return None

    vcvars_ver = f" -vcvars_ver={toolset}" if toolset and not ("/" in toolset or "\\" in toolset) else ""
    result = subprocess.run(
        f'cmd /c "call \"{vsdevcmd}\" -arch=x64{vcvars_ver} >nul 2>&1 && set"',
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
# Emscripten / emsdk environment setup
# ---------------------------------------------------------------------------

def find_emsdk_root(emsdk_path: str | None = None) -> Path | None:
    """Locate an emsdk installation directory, or None if none is found.

    Resolution order (the first that points at a real emsdk wins): the explicit
    `emsdk_path` (e.g. --emsdk-path), the `SC_EMSDK_PATH` env var, an already
    activated `EMSDK`, then deriving the root from `emcc` on PATH. This lets a
    developer use a bare emsdk checkout without permanently/system-activating it.
    """
    env_script = "emsdk_env.bat" if platform.system() == "Windows" else "emsdk_env.sh"

    candidates: list[Path] = []
    if emsdk_path:
        candidates.append(Path(emsdk_path))
    for var in ("SC_EMSDK_PATH", "EMSDK"):
        if os.environ.get(var):
            candidates.append(Path(os.environ[var]))
    emcc = shutil.which("emcc")
    if emcc:
        # Standard layout: <emsdk>/upstream/emscripten/emcc — the root is three levels up.
        parents = Path(emcc).resolve().parents
        if len(parents) >= 3:
            candidates.append(parents[2])

    for c in candidates:
        if (c / env_script).is_file():
            return c
    return None


def emsdk_toolchain_file(root: Path) -> Path:
    """Path to Emscripten's CMake toolchain file under an emsdk root."""
    return root / "upstream" / "emscripten" / "cmake" / "Modules" / "Platform" / "Emscripten.cmake"


def _emsdk_path_additions(root: Path) -> list[Path]:
    """Directories emsdk prepends to PATH: the SDK root, the emscripten tools
    (where emcc/em++ live), and emsdk's bundled node/python when present."""
    dirs = [root, root / "upstream" / "emscripten"]
    node_bins = sorted((root / "node").glob("*/bin"))
    if node_bins:
        dirs.append(node_bins[-1])  # newest installed node
    pythons = sorted((root / "python").glob("*"))
    if pythons:
        dirs.append(pythons[-1])
    return [d for d in dirs if d.is_dir()]


def _first_glob(root: Path, *patterns: str) -> Path | None:
    for pat in patterns:
        hits = sorted(root.glob(pat))
        if hits:
            return hits[-1]
    return None


def emsdk_env(emsdk_path: str | None = None) -> dict[str, str] | None:
    """Return a full environment dict with the Emscripten toolchain active, or None.

    The overlay is derived deterministically from the emsdk layout rather than by
    capturing emsdk's own activation script: that script emits shell-specific
    output (it detects bash vs cmd and may print `export ...` instead of mutating
    the cmd session), so capturing it via `set` silently dropped emsdk's PATH
    entries. Here we prepend the SDK dirs (including upstream/emscripten, where
    emcc lives) onto the inherited PATH and set EMSDK / EMSDK_NODE / EMSDK_PYTHON /
    EM_CONFIG ourselves, so a bare emsdk checkout works with no permanent or
    --system activation. Returns None when emsdk cannot be located.
    """
    root = find_emsdk_root(emsdk_path)
    if root is None:
        return None

    env = dict(os.environ)
    # Windows uses 'Path'; normalize onto whatever key the inherited env actually has.
    path_key = next((k for k in env if k.upper() == "PATH"), "PATH")
    additions = [str(d) for d in _emsdk_path_additions(root)]
    existing = env.get(path_key, "")
    env[path_key] = os.pathsep.join(additions + ([existing] if existing else []))

    env["EMSDK"] = str(root)
    node_exe = _first_glob(root, "node/*/bin/node.exe", "node/*/bin/node")
    if node_exe:
        env["EMSDK_NODE"] = str(node_exe)
    python_exe = _first_glob(root, "python/*/python.exe", "python/*/bin/python3", "python/*/python")
    if python_exe:
        env["EMSDK_PYTHON"] = str(python_exe)
    em_config = root / ".emscripten"
    if em_config.is_file():
        env["EM_CONFIG"] = str(em_config)
    return env


def env_for_preset(preset: Preset, emsdk_path: str | None = None) -> dict[str, str] | None:
    """Pick the subprocess environment a preset's commands need.

    Emscripten presets get the emsdk environment (emcc/node on PATH); every other
    preset falls back to the MSVC environment on Windows (None elsewhere, meaning
    "inherit the parent env unchanged"). Returned dicts are full environments, so
    callers pass them straight to run_step.
    """
    if preset.is_emscripten:
        return emsdk_env(emsdk_path)
    return msvc_env(preset.toolset)


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
    print(console.dim(f"[{_ts()}] [{step_type}]" + (f" {name}" if name else "")), file=sys.stderr)
    if verbose:
        print(console.dim(f"  $ {' '.join(cmd)}"), file=sys.stderr)

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
        print(
            console.red(f"  {label} TIMED OUT after {timeout:.0f}s (killed) in {duration_s * 1000:.0f} ms"),
            file=sys.stderr,
        )
    else:
        extra = ""
        if summary_extra is not None:
            try:
                extra = summary_extra(result) or ""
            except Exception:
                extra = ""
        verb = "succeeded" if result.ok else "failed"
        tint = console.green if result.ok else console.red
        print(tint(f"  {label} {verb}{extra} in {duration_s * 1000:.0f} ms"), file=sys.stderr)

    return result
