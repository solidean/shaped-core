#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///

"""Self-test for tools/dev/lib/pipeline/build_server.py — when a build goes through the optional build server, and when it falls back.

A fake client stands in for the server: a Python script answering `ensure` and `build` with the exit codes a test asks for.
The fallback paths are the ones that matter, since they run only on a machine where the server is missing or broken.
A failed compile through the server must stay a failed build, never a silent rebuild without it.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/build-server-self-test.py [-v]
"""

from __future__ import annotations

import functools
import os
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.core.models import StepResult  # noqa: E402
from tools.dev.lib.core.process import run_step  # noqa: E402
from tools.dev.lib.pipeline import build_server as bs  # noqa: E402

VERBOSE = "-v" in sys.argv

# The fake client: its behaviour comes from FAKE_SERVER (`ensure=<rc>,build=<rc>,failed`), and every call is appended to FAKE_SERVER_LOG.
FAKE = r'''
import os, sys
spec = dict(kv.split("=", 1) if "=" in kv else (kv, "1") for kv in os.environ.get("FAKE_SERVER", "").split(",") if kv)
with open(os.environ["FAKE_SERVER_LOG"], "a", encoding="utf-8") as f:
    f.write(" ".join(sys.argv[1:]) + "\n")
if sys.argv[1] == "ensure":
    sys.exit(int(spec.get("ensure", "0")))
print("[1/2] Building CXX object a.obj")
if "failed" in spec:
    print("FAILED: a.obj")
    print("a.cc(1): error: expected ';'", file=sys.stderr)
sys.exit(int(spec.get("build", "0")))
'''

PLAIN = ["cmake", "--build", "<dir>"]


class Fixture:
    """A build dir with a build.ninja, the fake client, and a `run` that executes server steps and records plain ones."""

    def __init__(self, root: Path, spec: str, *, ninja: bool = True, jobs: str | None = None) -> None:
        self.dir = root / "build"
        self.dir.mkdir()
        if ninja:
            (self.dir / "build.ninja").write_text("", encoding="utf-8")
        fake = root / "fake_server.py"
        fake.write_text(FAKE, encoding="utf-8")
        self.log = root / "calls.log"
        self.env = {**os.environ,"FAKE_SERVER": spec, "FAKE_SERVER_LOG": str(self.log)}
        if jobs is not None:
            self.env["CMAKE_BUILD_PARALLEL_LEVEL"] = jobs
        self.session = bs.Session([sys.executable, str(fake)], "install hint")
        self.plain_runs = 0
        self._step = functools.partial(run_step, step_type="build", name="all", build_dir=self.dir, cwd=root, env=self.env)

    def run(self, cmd: list[str]) -> StepResult:
        if cmd == PLAIN:
            self.plain_runs += 1
            return StepResult("build", "all", cmd, 0, 0.0, self.dir / "none.out", self.dir / "none.err")
        return self._step(cmd)

    def step(self, target=None, *, keep_going: bool = False) -> StepResult:
        return bs.build_step(self.session, self.dir, target, keep_going=keep_going, env=self.env, run=self.run, plain=PLAIN)

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []


def with_fixture(spec: str, **kw):
    def wrap(test):
        @functools.wraps(test)
        def run() -> None:
            with tempfile.TemporaryDirectory() as d:
                test(Fixture(Path(d), spec, **kw))
        return run
    return wrap


def test_no_server_wins_over_every_other_opt_in() -> None:
    on = bs.BuildServer(client=None, enabled=True)
    assert not bs.enabled(on, flag=False, environ={bs.ENV_ENABLE: "1"})
    assert bs.enabled(None, flag=True, environ={bs.ENV_ENABLE: "0"})
    assert not bs.enabled(on, flag=None, environ={bs.ENV_ENABLE: "0"}), "the environment over the project"
    assert bs.enabled(None, flag=None, environ={bs.ENV_ENABLE: "on"})
    assert bs.enabled(on, flag=None, environ={})
    assert not bs.enabled(bs.BuildServer(client=None), flag=None, environ={}), "off unless something opts in"
    assert bs.session(on, flag=False, environ={}) is None


def test_the_gate_stays_without_the_server_for_now() -> None:
    assert bs.GATE_USES_BUILD_SERVER is False


def test_a_missing_client_warns_once_and_builds_the_plain_way() -> None:
    with tempfile.TemporaryDirectory() as d:
        build = Path(d) / "build"
        build.mkdir()
        (build / "build.ninja").write_text("", encoding="utf-8")
        s = bs.session(bs.BuildServer(client=Path(d) / "absent.exe", install_hint="hint"), flag=True, environ={})
        assert s is not None and s.command is None
        plain: list[list[str]] = []

        def run(cmd: list[str]) -> StepResult:
            plain.append(cmd)
            return StepResult("build", "all", cmd, 0, 0.0, build / "o", build / "e")

        for _ in range(2):
            assert bs.build_step(s, build, None, keep_going=False, env={}, run=run, plain=PLAIN).ok
        assert plain == [PLAIN, PLAIN], plain
        assert len(s.warnings) == 1 and "absent.exe" in s.warnings[0] and "hint" in s.warnings[0], s.warnings


def test_the_client_override_names_the_executable() -> None:
    with tempfile.TemporaryDirectory() as d:
        exe = Path(d) / "client.exe"
        exe.write_bytes(b"")
        s = bs.session(bs.BuildServer(client=Path(d) / "absent.exe"), flag=True, environ={bs.ENV_CLIENT: str(exe)})
        assert s is not None and s.command == [str(exe)], s and s.command


def test_each_unavailable_code_from_ensure_falls_back() -> None:
    for code, why in bs.UNAVAILABLE.items():
        with tempfile.TemporaryDirectory() as d:
            f = Fixture(Path(d), f"ensure={code}")
            assert f.step().ok
            assert f.plain_runs == 1, (code, f.plain_runs)
            assert f.calls() == ["ensure --quiet"], f.calls()
            assert len(f.session.warnings) == 1 and why in f.session.warnings[0], f.session.warnings


@with_fixture("build=11")
def test_an_unavailable_code_from_build_falls_back(f: Fixture) -> None:
    assert f.step().ok
    assert f.plain_runs == 1
    assert "no server answers" in f.session.warnings[0], f.session.warnings


@with_fixture("build=1,failed")
def test_a_compile_error_is_a_failed_build_not_a_fallback(f: Fixture) -> None:
    r = f.step()
    assert r.returncode == 1, r
    assert f.plain_runs == 0, "never rebuilt without the server"
    assert not f.session.warnings, f.session.warnings
    assert "error: expected ';'" in r.stderr_log.read_text(encoding="utf-8"), "the compiler's output is in the step's log"


@with_fixture("build=1")
def test_a_failure_without_a_failed_edge_is_the_server_s_and_falls_back(f: Fixture) -> None:
    assert f.step().ok
    assert f.plain_runs == 1
    assert "without a compile error" in f.session.warnings[0], f.session.warnings


@with_fixture("build=130")
def test_a_cancelled_build_stops(f: Fixture) -> None:
    assert f.step().returncode == 130
    assert f.plain_runs == 0


@with_fixture("", jobs="8")
def test_a_served_build_passes_targets_jobs_and_keep_going(f: Fixture) -> None:
    assert f.step(["a", "b"], keep_going=True).ok
    assert f.step("c").ok
    assert f.plain_runs == 0
    calls = f.calls()
    assert calls[0] == "ensure --quiet" and calls.count("ensure --quiet") == 1, "ensure runs once a session"
    assert calls[1] == f"build {f.dir} -j 8 -k 0 a b", calls
    assert calls[2] == f"build {f.dir} -j 8 c", calls


@with_fixture("ensure=12")
def test_after_a_fallback_later_steps_build_plain_without_asking_again(f: Fixture) -> None:
    f.step()
    f.step()
    assert f.plain_runs == 2
    assert f.calls() == ["ensure --quiet"], f.calls()
    assert len(f.session.warnings) == 1


@with_fixture("", ninja=False)
def test_a_build_dir_without_build_ninja_builds_plain_silently(f: Fixture) -> None:
    assert f.step().ok
    assert f.plain_runs == 1 and f.calls() == [] and not f.session.warnings


def main() -> int:
    tests = [v for k, v in globals().items() if k.startswith("test_")]
    failures = 0
    for t in tests:
        try:
            t()
            if VERBOSE:
                print(f"ok   {t.__name__}")
        except AssertionError as e:
            failures += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"build-server-self-test: {len(tests) - failures} of {len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
