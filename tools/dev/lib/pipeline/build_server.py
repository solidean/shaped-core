"""An optional local build server: `dev.py build` and `dev.py test` hand their builds and test runs to it when a project or a developer opts in.

A build server keeps state between runs, such as a compile cache and test results, so a build or test run through it can be much faster.
It is never required: without it, or when it cannot serve, dev.py builds and tests exactly as it always has.

The client contract, which is all dev.py knows of a server:
- `<client> ensure --quiet` starts the server unless one answers, and exits 0 once one does;
- `<client> build <build-dir> [-j N] [-k N] [targets...]` builds like ninja, printing ninja's `[done/total]` lines and `FAILED:` blocks;
- exit 10 (another protocol version), 11 (no server answers), 12 (none could be started) or 13 (draining for a restart) means "not now";
- exit 1 with a `FAILED:` block is a failed build, 130 a cancelled one;
- `<client> test <build-dir> [names...] --json --timeout S [--threads N] [--rerun] [--speculate] [-v] [-- runner args]` runs those test binaries,
  serving a stored result instead of a run when nothing a binary executes or reads changed;
- `--speculate`, passed only when the project's `BuildServer.speculative_tests` says so, lets the server run those binaries again in the background
  after it rebuilds them on its own, with this request's arguments and environment; the same request without it withdraws that;
- it prints a JSON array on stdout, one object per binary: `name`, `path`, `status` (Passed, Failed, TimedOut, NoTests, Cancelled, Missing),
  `cached`, `verified`, `exit_code`, `ms`, `stored_ms`, `seed`, and `log`, a file with the binary's whole console output;
- each binary's JUnit report is `<binary>.results.xml` beside it, where dev.py keeps its own;
- exit 0, or 1 when any binary failed, with that array is a finished test run, 130 a cancelled one, 10-13 as above;
- `<client> proof <build-dir> --json --delta [-j N]` builds the dir and proves its source state from the server's records: every test green at least once,
  the ones without such a result run, then the ones most likely affected within a short budget;
  `--delta` judges the change a push sends: runs of its affected tests since it began count toward the budget,
  and a red blocks only when the change turned it red, the others listed in `filed` to be filed as issues;
  it prints one JSON object on stdout with `verdict` (`pass` or `fail`), `reasons`, `plan`, `ran`, `sideband` and `filed`, progress on stderr;
- exit 0 with that object is a proof that holds, 1 one that does not, 10-13 as above.

What opts in, the first that says either way: `--no-server`, `--server`, `SC_BUILD_SERVER=1|0`, the project's `BuildServer.enabled`.
A checkout's gitignored `dev.toml` sets the project's half through its `[build_server]` table (`from_dev_toml`): enabled, client, speculative_tests, ship_proof.
Background test runs are the project's choice alone, off unless `BuildServer.speculative_tests` is set.
`check`, and so `ship`, builds and runs its test suite through the server under the same opt-in, `check --no-server` and `ship --no-server` included.
`ship` proves through the server instead of running the gate's build and test steps only with the server opted in and
`ship --proof` or the project's `BuildServer.ship_proof`; any problem with the server runs the full gate instead.
`SC_BUILD_SERVER_CLIENT` names the client executable, over the project's `BuildServer.client`.

Public API:
    BuildServer             the project's half, a Policy field
    from_dev_toml(root)     the BuildServer a checkout's dev.toml asks for, over the project's defaults
    session(config, flag)   the Session a command builds through, or None when nothing opted in
    Session.build(...)      one build step through the server, or None when the caller must build the plain way
    Session.test(...)       test binaries run through the server, or None when the caller must run them the plain way
    Session.proof(...)      a build dir's source state proven through the server, or None when the caller must run the full gate
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, MutableMapping

from ..core import console, memory_guard, ui
from ..core.models import StepResult

ENV_ENABLE = "SC_BUILD_SERVER"
ENV_CLIENT = "SC_BUILD_SERVER_CLIENT"

# The exit codes that mean the server cannot serve right now, for both `ensure` and `build`.
UNAVAILABLE = {
    10: "it speaks another protocol version",
    11: "no server answers",
    12: "it could not be started",
    13: "it is draining for a restart",
}
CANCELLED = 130

_ENSURE_TIMEOUT_S = 60
_GENERIC_HINT = f"{ENV_CLIENT} names its client; --no-server builds without it"


@dataclass(frozen=True)
class BuildServer:
    """A project's build server: where its client is, how a developer installs it, and whether the project opts in by default."""

    # None, or a file that does not exist, means no server is installed.
    client: Path | None
    # Printed with the fallback warning: how to install or update the server.
    install_hint: str = _GENERIC_HINT
    enabled: bool = False
    # Whether the server may run this project's tests in the background after building on its own (`test --speculate`).
    speculative_tests: bool = False
    # Whether `ship` proves the tree through the server (`proof`) instead of running the gate's build and test steps.
    ship_proof: bool = False


# The dev.toml table a checkout sets its build server in.
DEV_TOML_TABLE = "build_server"


def dev_toml_table(root: Path) -> dict:
    """`<root>/dev.toml`'s `[build_server]` table; empty when the file or the table is absent, or the file does not parse."""
    try:
        data = tomllib.loads((root / "dev.toml").read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError):
        return {}
    table = data.get(DEV_TOML_TABLE)
    return table if isinstance(table, dict) else {}


def from_dev_toml(root: Path, *, client: Path | None = None, install_hint: str = _GENERIC_HINT) -> BuildServer:
    """The BuildServer `<root>/dev.toml`'s `[build_server]` asks for; `client` and `install_hint` are the project's defaults.

    `client = "<path>"` in the table overrides the project's client, a relative path taken from `root`.
    `enabled`, `speculative_tests` and `ship_proof` are on only when set to `true`; any other value is off.
    """
    table = dev_toml_table(root)
    named = table.get("client")
    if isinstance(named, str) and named:
        client = root / Path(named).expanduser()
    return BuildServer(
        client=client,
        install_hint=install_hint,
        enabled=table.get("enabled") is True,
        speculative_tests=table.get("speculative_tests") is True,
        ship_proof=table.get("ship_proof") is True,
    )


@dataclass(frozen=True)
class Proof:
    """A proof's verdict, its blockers one per line, and the client's whole report."""

    passed: bool
    reasons: list[str]
    report: dict


def _flag_word(value: str) -> bool | None:
    v = value.strip().lower()
    return True if v in ("1", "true", "on", "yes") else False if v in ("0", "false", "off", "no") else None


def enabled(config: BuildServer | None, *, flag: bool | None, environ: Mapping[str, str] = os.environ) -> bool:
    """Whether to build through the server: `flag` (False for --no-server, True for --server), then SC_BUILD_SERVER, then the project."""
    if flag is not None:
        return flag
    env = _flag_word(environ.get(ENV_ENABLE, ""))
    if env is not None:
        return env
    return bool(config and config.enabled)


def export(flag: bool | None, environ: MutableMapping[str, str] = os.environ) -> None:
    """Hands a command's `--server` / `--no-server` on as SC_BUILD_SERVER, to its own steps and to every dev.py it runs; None leaves it."""
    if flag is not None:
        environ[ENV_ENABLE] = "1" if flag else "0"


def session(config: BuildServer | None, *, flag: bool | None, environ: Mapping[str, str] = os.environ) -> Session | None:
    """The Session to build through, or None when nothing opted in; a missing client still gives a Session, which warns and falls back."""
    if not enabled(config, flag=flag, environ=environ):
        return None
    client = Path(environ[ENV_CLIENT]) if environ.get(ENV_CLIENT) else (config.client if config else None)
    hint = config.install_hint if config else _GENERIC_HINT
    speculative = bool(config and config.speculative_tests)
    if client is None or not client.is_file():
        return Session(None, hint, missing=f"no client at {client}" if client else "no client is configured", speculative_tests=speculative)
    return Session([str(client)], hint, speculative_tests=speculative)


def _failed_block(result: StepResult) -> bool:
    """Whether the step printed a `FAILED:` block, which tells a failed build from a server that failed."""
    for log in (result.stdout_log, result.stderr_log):
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if any(line.startswith("FAILED: ") for line in text.splitlines()):
            return True
    return False


def _last_line(text: str) -> str:
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    return lines[-1] if lines else ""


class Session:
    """The server for one dev.py run: `ensure` runs once, and once the server failed every later step builds the plain way."""

    def __init__(
        self, command: list[str] | None, install_hint: str, *, missing: str | None = None, speculative_tests: bool = False,
    ) -> None:
        self.command = command
        self.install_hint = install_hint
        self.speculative_tests = speculative_tests
        # None until the first build asks; then whether the server serves this run.
        self.available: bool | None = None
        self._missing = missing or "no client"
        self.warnings: list[str] = []

    def _warn(self, cause: str, *, doing: str = "building") -> None:
        text = f"warning: {doing} without the build server: {cause}. {self.install_hint}"
        self.warnings.append(text)
        ui.write_line(console.yellow(text))
        self.available = False

    def _ensure(self, env: Mapping[str, str] | None, *, doing: str = "building") -> bool:
        if self.available is not None:
            return self.available
        if self.command is None:
            self._warn(self._missing, doing=doing)
            return False
        # The server outlives this run, so it may leave the memory guard's job; only now, with this run building through it.
        memory_guard.allow_breakaway()
        try:
            # CREATE_NO_WINDOW: the server it starts must never open a console window, and the client must not hold ours.
            proc = subprocess.run(
                [*self.command, "ensure", "--quiet"], env=dict(env) if env is not None else None,
                stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=_ENSURE_TIMEOUT_S, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except OSError as e:
            self._warn(f"its client could not be started ({e.strerror or e})", doing=doing)
            return False
        except subprocess.TimeoutExpired:
            self._warn(f"`ensure` did not finish within {_ENSURE_TIMEOUT_S} s", doing=doing)
            return False
        if proc.returncode != 0:
            why = UNAVAILABLE.get(proc.returncode)
            detail = _last_line(proc.stderr) or _last_line(proc.stdout)
            self._warn(why or f"`ensure` failed (exit {proc.returncode}{': ' + detail if detail else ''})", doing=doing)
            return False
        self.available = True
        return True

    def build(
        self,
        build_dir: Path,
        target: str | list[str] | None,
        *,
        keep_going: bool,
        env: Mapping[str, str] | None,
        run: Callable[[list[str]], StepResult],
    ) -> StepResult | None:
        """Builds `target` in `build_dir` through the server with `run` (a run_step); None means build it the plain way.

        A failed or cancelled build is returned as it is, never retried: only a server that could not serve falls back.
        """
        if not (build_dir / "build.ninja").is_file():
            return None  # not a Ninja build dir: nothing a server could build, and no reason to warn
        if not self._ensure(env):
            return None
        assert self.command is not None
        targets = target if isinstance(target, list) else [target] if target else []
        cmd = [*self.command, "build", str(build_dir)]
        # cmake --build reads CMAKE_BUILD_PARALLEL_LEVEL; the server takes it as the build's own cap.
        jobs = (env if env is not None else os.environ).get("CMAKE_BUILD_PARALLEL_LEVEL", "").strip()
        if jobs.isdigit() and int(jobs) > 0:
            cmd += ["-j", jobs]
        if keep_going:
            cmd += ["-k", "0"]
        result = run([*cmd, *targets])
        rc = result.returncode
        if rc == 0 or rc == CANCELLED or (rc == 1 and _failed_block(result)):
            return result
        why = UNAVAILABLE.get(rc) or f"it failed without a compile error (exit {rc}; {result.stderr_log})"
        self._warn(f"{why}; building again without it")
        return None

    def test(
        self,
        build_dir: Path,
        names: list[str],
        *,
        runner_args: list[str],
        timeout: float | None,
        threads: int | None,
        rerun: bool,
        verbose: bool,
        env: Mapping[str, str] | None,
        run: Callable[[list[str]], StepResult],
    ) -> list[dict] | None:
        """Runs the test binaries `names` of `build_dir` through the server with `run` (a run_step); None means run them the plain way.

        Returns one result per binary, in the client's JSON shape.
        A failing test is a result like any other, never run again without the server: only a server that could not serve falls back.
        `timeout` holds for every binary named (None or 0: none), so a caller whose binaries differ in it asks once per timeout.
        """
        if not names or not self._ensure(env, doing="testing"):
            return None
        assert self.command is not None
        cmd = [*self.command, "test", str(build_dir), *names, "--json", "--timeout", f"{timeout or 0:g}"]
        if threads:
            cmd += ["--threads", str(threads)]
        if rerun:
            cmd.append("--rerun")
        if self.speculative_tests:
            cmd.append("--speculate")
        if verbose:
            cmd.append("-v")
        if runner_args:
            cmd += ["--", *runner_args]
        result = run(cmd)
        rc = result.returncode
        if rc in (0, 1, CANCELLED):
            results = _test_results(result.stdout_log)
            if results is not None and {r["name"] for r in results} >= set(names):
                return results
            if rc == CANCELLED:
                raise SystemExit(CANCELLED)  # cancelled before it reported: nothing to fall back to
        why = UNAVAILABLE.get(rc) or f"its test run ended without results (exit {rc}; {result.stderr_log})"
        self._warn(f"{why}; running the tests without it", doing="testing")
        return None

    def proof(self, build_dir: Path, *, env: Mapping[str, str] | None, progress: Callable[[str], None] = ui.write_line) -> Proof | None:
        """Builds `build_dir` and proves its source state through the server; None means the caller runs the full gate.

        A proof that does not hold is returned as it is: only a server that could not prove falls back.
        The client's progress goes to `progress` line by line, as it comes.
        """
        if not (build_dir / "build.ninja").is_file():
            self._warn(f"{build_dir} is not a configured Ninja build dir; running the full gate", doing="proving")
            return None
        if not self._ensure(env, doing="proving"):
            return None
        assert self.command is not None
        cmd = [*self.command, "proof", str(build_dir), "--json", "--delta"]
        jobs = (env if env is not None else os.environ).get("CMAKE_BUILD_PARALLEL_LEVEL", "").strip()
        if jobs.isdigit() and int(jobs) > 0:
            cmd += ["-j", jobs]
        try:
            # CREATE_NO_WINDOW, with both streams piped: the client and what it starts must never open a console window.
            proc = subprocess.Popen(
                cmd, env=dict(env) if env is not None else None, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except OSError as e:
            self._warn(f"its client could not be started ({e.strerror or e}); running the full gate", doing="proving")
            return None
        assert proc.stdout is not None and proc.stderr is not None
        stderr = proc.stderr
        relay = threading.Thread(target=lambda: [progress(line.rstrip("\n")) for line in stderr], daemon=True)
        relay.start()
        out = proc.stdout.read()
        rc = proc.wait()
        relay.join()
        if rc in (0, 1):
            try:
                report = json.loads(out)
            except ValueError:
                report = None
            if isinstance(report, dict) and report.get("verdict") in ("pass", "fail"):
                return Proof(passed=rc == 0 and report["verdict"] == "pass", reasons=list(report.get("reasons") or []), report=report)
        why = UNAVAILABLE.get(rc) or f"its proof ended without a verdict (exit {rc})"
        self._warn(f"{why}; running the full gate", doing="proving")
        return None


def _test_results(stdout_log: Path) -> list[dict] | None:
    """The per-binary results the client printed, as one JSON array or one object per line; None when there are none to read."""
    try:
        text = stdout_log.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None
    try:
        doc = json.loads(text)
        items = doc if isinstance(doc, list) else [doc]
    except ValueError:
        try:
            items = [json.loads(line) for line in text.splitlines() if line.strip()]
        except ValueError:
            return None
    if not items or not all(isinstance(r, dict) and "name" in r and "status" in r for r in items):
        return None
    return items


def build_step(
    server: Session | None,
    build_dir: Path,
    target: str | list[str] | None,
    *,
    keep_going: bool,
    env: Mapping[str, str] | None,
    run: Callable[[list[str]], StepResult],
    plain: list[str],
) -> StepResult:
    """One build step: through `server` when there is one and it serves, else `plain` (the cmake --build command)."""
    if server is not None:
        result = server.build(build_dir, target, keep_going=keep_going, env=env, run=run)
        if result is not None:
            return result
    return run(plain)
