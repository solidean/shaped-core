"""Subprocess execution: MSVC and emsdk environment setup, and the quiet-by-default run_step.

`run_step` is the single choke point for every external command the tooling runs.
It captures stdout/stderr to per-step log files, optionally mirroring them live, and returns a StepResult describing the outcome.
"""

from __future__ import annotations

import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path

from . import console
from .logs import report_capture, step_log_paths
from .models import Preset, StepResult


def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


_mirror_test_output = False


def configure_mirroring(*, mirror_test_output: bool = False) -> None:
    """Mirror every "test" step live, whatever its caller passed for `mirror`.

    Process-wide presentation, set once from the CLI like console.configure, and `run_step` is the only reader.
    Setting it here rather than at each dev.test() call site also covers the paths that run test binaries without going through one, such as `coverage run` and `pgo train`.
    Mirroring is additive to capture, so this never affects the logs.
    """
    global _mirror_test_output
    _mirror_test_output = mirror_test_output


# ---------------------------------------------------------------------------
# MSVC environment setup (Windows only)
# ---------------------------------------------------------------------------

def _find_vsdevcmd(toolset: str | None = None) -> str | None:
    """Locate VsDevCmd.bat via vswhere or well-known paths.

    With `toolset` set — a bare version like "14.51", not a path — pick the instance whose VC\\Tools\\MSVC actually has that toolset, prerelease included.
    Without one, use the latest instance, then fall back to the known VS 2022 install paths.
    """
    if toolset is not None and not ("/" in toolset or "\\" in toolset):
        # Function-local: keeps msvc env setup off core's import path (toolchain sits above core).
        from ..toolchain.toolset import find_msvc_instance

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


def msvc_env(toolset: str | None = None, arch: str = "x64") -> dict[str, str] | None:
    """Return an environment dict with MSVC tools on PATH, or None if not needed.

    Returns None on non-Windows, or when VsDevCmd.bat cannot be found.
    With no `toolset`, an already-on-PATH cl.exe is taken as-is and None is returned to inherit the env.
    With one pinned, the ambient cl is ignored: the matching Visual Studio instance is located and `-vcvars_ver=<toolset>` selects that exact toolset.
    A path-valued toolset never reaches here — it is the clang/gcc compiler-override path's.
    `arch` selects the vcvars target architecture, so an arm64 preset gets the arm64 toolchain.
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
        f'cmd /c "call \"{vsdevcmd}\" -arch={arch}{vcvars_ver} >nul 2>&1 && set"',
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

    First match wins, in order: the explicit `emsdk_path` (--emsdk-path), `SC_EMSDK_PATH`, an already activated `EMSDK`, then the root derived from `emcc` on PATH.
    A bare emsdk checkout therefore works without permanent or --system activation.
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
    """Directories emsdk prepends to PATH: the SDK root, the emscripten tools where emcc and em++ live, and emsdk's bundled node/python when present."""
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

    The overlay is derived from the emsdk layout rather than by capturing emsdk's own activation script, whose output is shell-specific and cannot be read back reliably.
    So the SDK dirs are prepended onto the inherited PATH and EMSDK / EMSDK_NODE / EMSDK_PYTHON / EM_CONFIG are set here.
    Returns None when emsdk cannot be located.
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

    Emscripten presets get the emsdk environment; every other preset falls back to the MSVC environment on Windows, and to None elsewhere.
    None means "inherit the parent env unchanged"; a returned dict is a full environment, ready to hand straight to run_step.
    """
    if preset.is_emscripten:
        return emsdk_env(emsdk_path)
    return msvc_env(preset.toolset, preset.arch)


# ---------------------------------------------------------------------------
# Response files
# ---------------------------------------------------------------------------

# Windows caps a command line at 32767 chars (CreateProcess). Stay well under it: the
# limit counts the exe path and every flag too, not just the file list.
_ARGV_LIMIT = 30000


@contextmanager
def response_file(args: list[str], prefix: str) -> Iterator[list[str]]:
    """Yield the argv tail to pass for `args`, spilling to a response file when too long.

    LLVM tools (clang-format, llvm-nm, llvm-objdump) expand `@file`, one argument per line.
    A full-tree file list blows past the Windows command-line limit, so anything over _ARGV_LIMIT chars is written to a temp file and passed as a single `@path`.
    A short list is yielded verbatim, which keeps real paths in the step logs for repro — an `@C:\\...\\tmp.rsp` there would point at an already-deleted file.
    LLVM tokenizes response files Windows-style on Windows hosts, so native backslash paths need no escaping.
    POSIX hosts use forward slashes, where GNU tokenization's backslash-escaping never triggers.
    """
    if sum(len(a) + 1 for a in args) <= _ARGV_LIMIT:
        yield list(args)
        return

    # delete=False + explicit unlink: Windows cannot reopen an open NamedTemporaryFile.
    tmp = tempfile.NamedTemporaryFile("w", suffix=".rsp", prefix=prefix, delete=False, encoding="utf-8")
    try:
        with tmp:
            tmp.write("\n".join(args))
        yield ["@" + tmp.name]
    finally:
        try:
            os.unlink(tmp.name)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Step runner
# ---------------------------------------------------------------------------

def _pump(src, log_file, mirror_to) -> None:
    """Read `src` line by line, writing to `log_file` and (optionally) `mirror_to`.

    This loop must not die while the child is alive.
    Nothing else drains the pipe, so a pump thread that raises leaves the child blocked on a full OS pipe buffer forever, and `proc.wait()` with it.
    So every per-line failure is reported once and swallowed, and draining continues to the end of the stream.
    """
    reported = False

    def complain(what: str, exc: BaseException) -> None:
        nonlocal reported
        if reported:
            return # one line, not one per output line
        reported = True
        try:
            print(f"warning: dev.py could not {what}: {type(exc).__name__}: {exc}\n"
                  f"         output continues, but some of it may be missing or replaced "
                  f"(the run log has the full text).", file=sys.stderr, flush=True)
        except Exception:
            pass # the complaint channel itself is broken; there is nowhere left to say so

    try:
        for line in iter(src.readline, ""):
            try:
                log_file.write(line)
            except Exception as e:
                complain("write to the run log", e)
            if mirror_to is not None:
                try:
                    mirror_to.write(line)
                    mirror_to.flush()
                except Exception as e:
                    complain("mirror child output to this terminal", e)
    except Exception as e:
        complain("read the child's output", e)
    finally:
        try:
            src.close()
        except Exception:
            pass


# How long a timed-out child gets to report where it is before it is killed outright.
# Long enough for a crash handler to symbolize every thread and write, short enough to be noise against any real timeout.
_CRASH_REPORT_GRACE_S = 2.0


def _request_crash_report(proc: subprocess.Popen) -> bool:
    """Ask a hung child to say where it is, and report whether the request went out.

    A killed process says nothing, and a timeout with no stack is the hardest failure to chase.
    Our binaries already know how to answer, because nexus installs cc::install_crash_handler().
    On a fatal fault it prints the running test, the faulting thread's stack, and every other thread's — including the one that is actually stuck.
    So provoke that report and let it reach the stderr we are already capturing, rather than terminating the process mid-hang and learning nothing.

    Best-effort throughout: a child that ignores the request, or is not one of ours, is killed as before.
    """
    if os.name == "nt":
        try:
            import ctypes

            # A remote thread with a NULL entry point faults on its first instruction, and that fault is genuinely unhandled, so it reaches the top-level SetUnhandledExceptionFilter.
            # DebugBreakProcess does NOT work here, and is the obvious-looking trap.
            # ntdll's DbgUiRemoteBreakin wraps its DbgBreakPoint in an __except of its own.
            # So with no debugger attached the breakpoint is handled, never becomes unhandled, and the filter never runs.
            #
            # The injected thread's own frames say nothing; the running-test hook and the per-thread walk are what carry the answer, and both are process-wide.
            # The child usually dies of the poke, since the handler returns EXCEPTION_EXECUTE_HANDLER and lets the fault run its course, so the caller's kill is only the fallback.
            kernel32 = ctypes.windll.kernel32
            handle = kernel32.OpenProcess(0x1F0FFF, False, proc.pid) # PROCESS_ALL_ACCESS
            if not handle:
                return False
            try:
                thread = kernel32.CreateRemoteThread(handle, None, 0, None, None, 0, None)
                if not thread:
                    return False
                kernel32.CloseHandle(thread)
                return True
            finally:
                kernel32.CloseHandle(handle)
        except Exception:
            return False

    # POSIX: the handler's signal path does the same job.
    try:
        proc.send_signal(signal.SIGABRT)
        return True
    except Exception:
        return False


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

    `step_type` is the kind of step ("configure"/"build"/"test") and `name` the specific thing it acts on — a target, "all", or a test binary.
    Prints a one-line `[ts] [step_type] name` banner, captures both streams under build_dir/run-logs/, then prints capture pointers and a pass/fail summary.
    Streams are mirrored live when `mirror`, or on a "test" step once configure_mirroring enabled it.
    `summary_extra`, when given, is called with the finished StepResult and its return value is appended to the summary line.
    It is skipped on timeout, and any exception it raises is swallowed.
    On timeout the process is killed and the step reports returncode 124, the conventional timeout code.
    """
    mirror = mirror or (_mirror_test_output and step_type == "test")

    print(console.dim(f"[{_ts()}] [{step_type}]" + (f" {name}" if name else "")), file=sys.stderr)
    if verbose:
        print(console.dim(f"  $ {' '.join(cmd)}"), file=sys.stderr)

    stdout_path, stderr_path = step_log_paths(build_dir, step_type, name)

    start = time.perf_counter()
    timed_out = False
    asked = False
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
            # Ask where it is before killing it — see _request_crash_report.
            asked = _request_crash_report(proc)
            if asked:
                try:
                    proc.wait(timeout=_CRASH_REPORT_GRACE_S)
                except subprocess.TimeoutExpired:
                    pass
            if proc.poll() is None:
                proc.kill()
            proc.wait()
        for t in threads:
            t.join()
        if timed_out:
            err_f.write(f"\n[dev.py] TIMEOUT: '{name or step_type}' exceeded {timeout:.0f}s, then killed.\n")
            if asked:
                # Say this plainly: the report above announces a fatal fault that never really happened,
                # and a reader who takes it at face value chases the wrong thing entirely.
                err_f.write("[dev.py] Any 'fatal crash' report above is INDUCED — the process was faulted on\n"
                            "[dev.py] purpose so it would say where it was. The faulting thread is dev.py's\n"
                            "[dev.py] doing and means nothing; the hung code is under 'other threads'.\n")
            else:
                err_f.write("[dev.py] Could not ask it for a crash report, so there is no stack above.\n")
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
        code_note = "" if result.ok else _describe_exit_code(returncode)
        tint = console.green if result.ok else console.red
        print(tint(f"  {label} {verb}{extra}{code_note} in {duration_s * 1000:.0f} ms"), file=sys.stderr)

    return result


# Windows surfaces a fatal SEH fault as the NTSTATUS exception code, returned signed by subprocess; POSIX returns the negated terminating signal.
# Name the common crash codes so a bare non-zero exit reads as "access violation" rather than an opaque number.
_NTSTATUS_NAMES = {
    0xC0000005: "access violation (segfault)",
    0x80000003: "breakpoint / __debugbreak",
    0xC000001D: "illegal instruction",
    0xC00000FD: "stack overflow",
    0xC0000094: "integer divide by zero",
    0xC0000409: "stack buffer overrun / fail-fast",
    0xC0000374: "heap corruption",
}
_SIGNAL_NAMES = {4: "SIGILL", 6: "SIGABRT", 7: "SIGBUS", 8: "SIGFPE", 11: "SIGSEGV"}


def _describe_exit_code(rc: int) -> str:
    """Human-readable suffix for a non-zero process exit, empty for clean exits."""
    if rc == 0:
        return ""
    if -64 <= rc < 0:  # POSIX: negative small value is the terminating signal
        sig = -rc
        name = _SIGNAL_NAMES.get(sig)
        return f" (signal {sig}: {name})" if name else f" (signal {sig})"
    u = rc & 0xFFFFFFFF  # Windows NTSTATUS crash codes arrive as signed 32-bit
    name = _NTSTATUS_NAMES.get(u)
    if name is not None:
        return f" (exit 0x{u:08X}: {name})"
    return f" (exit code {rc})"
