"""An optional local build server: `dev.py build` and `dev.py test` hand their builds to it when a project or a developer opts in.

A build server keeps state between builds, such as a compile cache, so a build that goes through it can be much faster than ninja alone.
It is never required: without it, or when it cannot serve, dev.py builds exactly as it always has.

The client contract, which is all dev.py knows of a server:
- `<client> ensure --quiet` starts the server unless one answers, and exits 0 once one does;
- `<client> build <build-dir> [-j N] [-k N] [targets...]` builds like ninja, printing ninja's `[done/total]` lines and `FAILED:` blocks;
- exit 10 (another protocol version), 11 (no server answers), 12 (none could be started) or 13 (draining for a restart) means "not now";
- exit 1 with a `FAILED:` block is a failed build, 130 a cancelled one.

What opts in, the first that says either way: `--no-server`, `--server`, `SC_BUILD_SERVER=1|0`, the project's `BuildServer.enabled`.
`SC_BUILD_SERVER_CLIENT` names the client executable, over the project's `BuildServer.client`.

Public API:
    BuildServer             the project's half, a Policy field
    session(config, flag)   the Session a command builds through, or None when nothing opted in
    Session.build(...)      one build step through the server, or None when the caller must build the plain way
"""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping

from ..core import console, ui
from ..core.models import StepResult

ENV_ENABLE = "SC_BUILD_SERVER"
ENV_CLIENT = "SC_BUILD_SERVER_CLIENT"

# Whether `check` (and so `ship`) builds through the server; the gate stays independent of it until the server has proven itself.
# Flipping this is the whole switch: every gate build already asks Context.build_server(gate=True).
# TEMPORARY(debt-devpy-gate-without-build-server)
GATE_USES_BUILD_SERVER = False

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


def session(config: BuildServer | None, *, flag: bool | None, environ: Mapping[str, str] = os.environ) -> Session | None:
    """The Session to build through, or None when nothing opted in; a missing client still gives a Session, which warns and falls back."""
    if not enabled(config, flag=flag, environ=environ):
        return None
    client = Path(environ[ENV_CLIENT]) if environ.get(ENV_CLIENT) else (config.client if config else None)
    hint = config.install_hint if config else _GENERIC_HINT
    if client is None or not client.is_file():
        return Session(None, hint, missing=f"no client at {client}" if client else "no client is configured")
    return Session([str(client)], hint)


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

    def __init__(self, command: list[str] | None, install_hint: str, *, missing: str | None = None) -> None:
        self.command = command
        self.install_hint = install_hint
        # None until the first build asks; then whether the server serves this run.
        self.available: bool | None = None
        self._missing = missing or "no client"
        self.warnings: list[str] = []

    def _warn(self, cause: str) -> None:
        text = f"warning: building without the build server: {cause}. {self.install_hint}"
        self.warnings.append(text)
        ui.write_line(console.yellow(text))
        self.available = False

    def _ensure(self, env: Mapping[str, str] | None) -> bool:
        if self.available is not None:
            return self.available
        if self.command is None:
            self._warn(self._missing)
            return False
        try:
            # CREATE_NO_WINDOW: the server it starts must never open a console window, and the client must not hold ours.
            proc = subprocess.run(
                [*self.command, "ensure", "--quiet"], env=dict(env) if env is not None else None,
                stdin=subprocess.DEVNULL, capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=_ENSURE_TIMEOUT_S, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except OSError as e:
            self._warn(f"its client could not be started ({e.strerror or e})")
            return False
        except subprocess.TimeoutExpired:
            self._warn(f"`ensure` did not finish within {_ENSURE_TIMEOUT_S} s")
            return False
        if proc.returncode != 0:
            why = UNAVAILABLE.get(proc.returncode)
            detail = _last_line(proc.stderr) or _last_line(proc.stdout)
            self._warn(why or f"`ensure` failed (exit {proc.returncode}{': ' + detail if detail else ''})")
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
