#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.12"
# ///

"""Self-test for tools/dev/lib/pipeline/build_server.py — when a build or test run goes through the optional build server, and when it falls back.

A fake client stands in for the server: a Python script answering `ensure`, `build`, `test` and `proof` with the exit codes and results a test asks for.
`ship`'s proof mode is tested here too: when it asks the server, and that a proof stands in for the gate's build and test steps only while it can.
The fallback paths are the ones that matter, since they run only on a machine where the server is missing or broken.
A failed compile through the server must stay a failed build, and a failed test a failed test, never a silent rerun without it.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/build-server-self-test.py [-v]
"""

from __future__ import annotations

import functools
import json
import os
import sys
import tempfile
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.core import crash_reports  # noqa: E402
from tools.dev.lib.core.models import StepResult, Target  # noqa: E402
from tools.dev.lib.core.process import run_step  # noqa: E402
from tools.dev.lib.pipeline import build_server as bs  # noqa: E402
from tools.dev.lib.pipeline import test as pipeline_test  # noqa: E402

VERBOSE = "-v" in sys.argv

# The fake client: its behaviour comes from FAKE_SERVER (`ensure=<rc>,build=<rc>,failed`), and every call is appended to FAKE_SERVER_LOG.
# Under `test` it answers from FAKE_TESTS, a JSON object of binary name to the result's fields plus `out`, the binary's console output.
# Under `proof` it prints FAKE_PROOF, a report, and exits by its verdict, or exits `proof=<rc>` without one.
FAKE = r'''
import json, os, sys
from pathlib import Path
spec = dict(kv.split("=", 1) if "=" in kv else (kv, "1") for kv in os.environ.get("FAKE_SERVER", "").split(",") if kv)
with open(os.environ["FAKE_SERVER_LOG"], "a", encoding="utf-8") as f:
    f.write(" ".join(sys.argv[1:]) + "\n")
if sys.argv[1] == "ensure":
    sys.exit(int(spec.get("ensure", "0")))
if sys.argv[1] == "proof":
    print("fake: proving", file=sys.stderr)
    if "proof" in spec:
        sys.exit(int(spec["proof"]))
    report = os.environ["FAKE_PROOF"]
    print(report)
    sys.exit(0 if json.loads(report)["verdict"] == "pass" else 1)
if sys.argv[1] == "test":
    if "test" in spec:
        print("fake: the server failed", file=sys.stderr)
        sys.exit(int(spec["test"]))
    build, words = Path(sys.argv[2]), sys.argv[3:]
    names = words[:words.index("--json")]
    answers, results = json.loads(os.environ["FAKE_TESTS"]), []
    for name in names:
        a = dict(answers[name])
        exe = build / f"{name}.exe"
        log = build / f"{name}.exe.server.log"
        log.write_text(a.pop("out", ""), encoding="utf-8")
        if a.get("status", "Passed") == "Passed":
            (build / f"{name}.exe.results.xml").write_text(
                '<testsuites><testsuite name="s" tests="2" failures="0"><testcase name="a"/><testcase name="b"/>'
                "</testsuite></testsuites>", encoding="utf-8")
        results.append({"name": name, "path": str(exe), "status": "Passed", "cached": False, "verified": None,
                        "exit_code": 0, "ms": 5, "stored_ms": 0, "seed": 7, "log": str(log), **a})
    print(json.dumps(results, indent=2))
    print("fake: done", file=sys.stderr)
    sys.exit(1 if any(r["status"] not in ("Passed", "NoTests") for r in results) else 0)
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


def test_dev_toml_sets_the_project_s_half() -> None:
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        default = Path("default.exe")
        assert bs.from_dev_toml(root, client=default) == bs.BuildServer(client=default), "no dev.toml keeps the defaults"
        (root / "dev.toml").write_text('[build_server]\nenabled = true\nclient = "bin/server.exe"\nship_proof = "yes"\n', encoding="utf-8")
        c = bs.from_dev_toml(root, client=default, install_hint="hint")
        assert c.enabled and c.client == root / "bin" / "server.exe" and c.install_hint == "hint", c
        assert not c.ship_proof and not c.speculative_tests, "only `true` turns a switch on"
        (root / "dev.toml").write_text("[build_server\n", encoding="utf-8")
        assert bs.from_dev_toml(root, client=default) == bs.BuildServer(client=default), "a dev.toml that does not parse is ignored"


def test_no_server_wins_over_every_other_opt_in() -> None:
    on = bs.BuildServer(client=None, enabled=True)
    assert not bs.enabled(on, flag=False, environ={bs.ENV_ENABLE: "1"})
    assert bs.enabled(None, flag=True, environ={bs.ENV_ENABLE: "0"})
    assert not bs.enabled(on, flag=None, environ={bs.ENV_ENABLE: "0"}), "the environment over the project"
    assert bs.enabled(None, flag=None, environ={bs.ENV_ENABLE: "on"})
    assert bs.enabled(on, flag=None, environ={})
    assert not bs.enabled(bs.BuildServer(client=None), flag=None, environ={}), "off unless something opts in"
    assert bs.session(on, flag=False, environ={}) is None


def test_the_gate_follows_the_same_opt_in_and_its_flags_reach_child_runs() -> None:
    from tools.dev.cmd import args as a
    from tools.dev.cmd.context import Context
    ctx = types.SimpleNamespace(policy=types.SimpleNamespace(build_server=bs.BuildServer(client=None, enabled=True)))
    saved = os.environ.pop(bs.ENV_ENABLE, None)
    try:
        assert Context.build_server(ctx, gate=True) is not None, "an opted-in project's gate builds through the server"
        bs.export(a.server_flag(types.SimpleNamespace(no_server=True, server=False)))
        assert os.environ[bs.ENV_ENABLE] == "0" and Context.build_server(ctx, gate=True) is None, "check --no-server"
        bs.export(a.server_flag(types.SimpleNamespace(no_server=False, server=False)))
        assert os.environ[bs.ENV_ENABLE] == "0", "no flag leaves what the parent run set"
        bs.export(True)
        off = types.SimpleNamespace(policy=types.SimpleNamespace(build_server=bs.BuildServer(client=None)))
        assert Context.build_server(off, gate=True) is not None, "check --server"
    finally:
        os.environ.pop(bs.ENV_ENABLE, None)
        if saved is not None:
            os.environ[bs.ENV_ENABLE] = saved


def test_a_request_s_binaries_count_its_wall_time_once_and_plain_ones_their_own() -> None:
    from tools.dev.lib.core import report
    served = lambda s, req: {"duration_s": s, "build_server": {"cached": False, "request": req, "request_s": 2.0}}  # noqa: E731
    records = [served(5.0, 1), served(7.0, 1), served(3.0, 2), {"duration_s": 1.5}]
    assert report.wall_s(records) == 2.0 + 2.0 + 1.5, "one wall time per request, side by side; a plain run's own"
    assert report.wall_s([{"duration_s": 1.0, "build_server": {"cached": True}}]) == 1.0, "a record from before requests were timed"


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


# --- test runs through the server ---


class TestFixture:
    """A build dir with fake test binaries, the fake client answering `test` from `answers`, and the preset dev.py would pass."""

    def __init__(self, root: Path, answers: dict, spec: str = "", timeouts: dict | None = None) -> None:
        self.dir = root / "build"
        self.dir.mkdir()
        fake = root / "fake_server.py"
        fake.write_text(FAKE, encoding="utf-8")
        self.log = root / "calls.log"
        self.env = {**os.environ, "FAKE_SERVER": spec, "FAKE_SERVER_LOG": str(self.log), "FAKE_TESTS": json.dumps(answers)}
        self.session = bs.Session([sys.executable, str(fake)], "install hint")
        self.preset = types.SimpleNamespace(name="fake", build_dir=self.dir, is_cross_compiling=False, is_emscripten=False)
        self.targets = []
        for name in answers:
            exe = self.dir / f"{name}.exe"
            exe.write_bytes(b"")
            self.targets.append(Target(name, "EXECUTABLE", exe, ("tests",), (timeouts or {}).get(name)))
        self.root = root

    def run(self, *, test_name=None, extra_args=(), timeout=60.0, explicit=False, rerun=False):
        return pipeline_test._test_through_server(
            self.session, self.preset, self.targets, root=self.root, test_name=test_name, extra_args=list(extra_args),
            env=self.env, timeout=timeout, timeout_is_explicit=explicit, watchdog_fraction=0.5, rerun=rerun,
            mirror=False, verbose=False,
        )

    def calls(self) -> list[str]:
        return [c for c in self.log.read_text(encoding="utf-8").splitlines() if c != "ensure --quiet"]


def with_tests(answers: dict, spec: str = "", **kw):
    def wrap(test):
        @functools.wraps(test)
        def run() -> None:
            with tempfile.TemporaryDirectory() as d:
                test(TestFixture(Path(d), answers, spec, **kw))
        return run
    return wrap


@with_tests({"a": {"cached": True, "stored_ms": 2500}, "b": {}})
def test_tests_run_through_the_server_marked_served_or_ran(f: TestFixture) -> None:
    records, left = f.run(extra_args=["--thorough"])
    assert not left and not f.session.warnings, (left, f.session.warnings)
    assert [r["name"] for r in records] == ["a", "b"], records
    assert all(r["returncode"] == 0 for r in records), records
    assert records[0]["build_server"]["cached"] is True and records[1]["build_server"]["cached"] is False, records
    assert records[0]["junit"]["tests"] == 2, "the JUnit report beside the binary is read as from a plain run"
    assert f.calls() == [f"test {f.dir} a b --json --timeout 60 -- --watchdog 30 --thorough"], f.calls()
    assert "--seed 7" in " ".join(records[0]["command"]), "the record's command reproduces the run, seed and all"


@with_tests({"a": {}, "bad": {"status": "Failed", "exit_code": 3, "out": "check failed at bad.exe+0x1f20\n"}})
def test_a_failing_binary_is_a_failure_with_its_log_sites_and_kept_folder(f: TestFixture) -> None:
    records, left = f.run()
    assert not left and not f.session.warnings, "a test failure is never run again without the server"
    bad = records[1]
    assert bad["name"] == "bad" and bad["returncode"] == 3, bad
    stdout = f.dir / bad["stdout_log"]
    stderr = f.dir / bad["stderr_log"]
    assert "bad.exe+0x1f20" in stdout.read_text(encoding="utf-8"), "the binary's output is in dev.py's run log"
    err = stderr.read_text(encoding="utf-8")
    assert "the sites above, resolved" in err and "bad.exe+0x1f20" in err, err
    kept = list(crash_reports.crash_root(f.dir).glob("bad-*-failed"))
    assert len(kept) == 1 and (kept[0] / stdout.name).is_file(), "the failed run's logs are kept for test_diag"


@with_tests({"slow": {"status": "TimedOut", "exit_code": None}, "gone": {"status": "Missing", "exit_code": None}})
def test_a_timeout_and_a_missing_binary_fail_as_in_a_plain_run(f: TestFixture) -> None:
    records, left = f.run()
    assert not left
    assert records[0]["returncode"] == 124 and records[0]["timed_out"], records[0]
    assert records[1]["returncode"] == 127, records[1]


@with_tests({"a": {"status": "NoTests"}, "b": {}})
def test_a_binary_whose_filter_selected_nothing_is_skipped(f: TestFixture) -> None:
    records, _ = f.run(test_name="foo")
    assert [r["name"] for r in records] == ["b"], records
    assert f.calls()[0].endswith("-- foo --watchdog 30"), f.calls()


def test_each_unavailable_code_from_test_falls_back() -> None:
    for code in [*bs.UNAVAILABLE, 2]:
        with tempfile.TemporaryDirectory() as d:
            f = TestFixture(Path(d), {"a": {}, "b": {}}, f"test={code}")
            records, left = f.run()
            assert records == [] and [t.name for t in left] == ["a", "b"], (code, records, left)
            assert len(f.session.warnings) == 1 and "testing without" in f.session.warnings[0], f.session.warnings


@with_tests({"a": {}}, "ensure=11")
def test_a_server_that_cannot_start_runs_every_test_plain(f: TestFixture) -> None:
    records, left = f.run()
    assert records == [] and len(left) == 1
    assert f.calls() == [] and "no server answers" in f.session.warnings[0], f.session.warnings


def test_a_missing_client_warns_that_it_tests_without_it() -> None:
    s = bs.session(bs.BuildServer(client=Path("absent.exe")), flag=True, environ={})
    assert s is not None
    assert s.test(Path("."), ["a"], runner_args=[], timeout=60, threads=None, rerun=False, verbose=False, env={},
                  run=lambda cmd: None) is None
    assert "testing without the build server" in s.warnings[0], s.warnings


@with_tests({"a": {}, "long": {}}, timeouts={"long": 300.0})
def test_binaries_of_another_timeout_go_in_a_request_of_their_own(f: TestFixture) -> None:
    records, _ = f.run(extra_args=["--jobs", "4"], rerun=True)
    assert len(records) == 2
    assert f.calls() == [
        f"test {f.dir} a --json --timeout 60 --threads 4 --rerun -- --watchdog 30",
        f"test {f.dir} long --json --timeout 300 --threads 4 --rerun -- --watchdog 150",
    ], f.calls()


@with_tests({"a": {}})
def test_background_test_runs_are_asked_for_only_when_the_project_says_so(f: TestFixture) -> None:
    f.run()
    assert "--speculate" not in f.calls()[0], f.calls()
    f.session.speculative_tests = True
    f.run()
    assert f.calls()[1] == f"test {f.dir} a --json --timeout 60 --speculate -- --watchdog 30", f.calls()
    s = bs.session(bs.BuildServer(client=Path(sys.executable), speculative_tests=True), flag=True, environ={})
    assert s is not None and s.speculative_tests


def test_runs_with_side_outputs_bypass_the_server() -> None:
    native = types.SimpleNamespace(is_cross_compiling=False, is_emscripten=False)
    wasm = types.SimpleNamespace(is_cross_compiling=True, is_emscripten=True)
    plain = dict(sample=False, write_xml=True, extra_env=False)
    assert pipeline_test._server_bypass(preset=native, **plain) is None
    assert pipeline_test._server_bypass(preset=wasm, **plain)
    assert pipeline_test._server_bypass(preset=native, **{**plain, "sample": True}) == "--sample"
    assert pipeline_test._server_bypass(preset=native, **{**plain, "write_xml": False})
    assert pipeline_test._server_bypass(preset=native, **{**plain, "extra_env": True})


def _report(verdict: str, *, reasons: list[str] | None = None, over_budget: int = 0, must_run: int = 0) -> dict:
    """A proof report in the client's shape, as much of it as dev.py reads."""
    must = [{"binary": "b", "test": f"t{i}", "score": 0.0, "est_ms": 2000} for i in range(must_run)]
    over = [{"binary": "b", "test": f"o{i}", "score": 0.5, "est_ms": 2000} for i in range(over_budget)]
    ran = [{"binary": "b", "test": t["test"], "why": "must run", "outcome": "pass" if verdict == "pass" else "fail",
            "served": False, "ms": 1} for t in must]
    return {"verdict": verdict, "reasons": reasons or [], "declared": 10, "plan": {"must_run": must, "over_budget": over},
            "ran": ran, "sideband": []}


def _prove(f: Fixture, report: dict | None) -> bs.Proof | None:
    if report is not None:
        f.env["FAKE_PROOF"] = json.dumps(report)
    lines: list[str] = []
    proof = f.session.proof(f.dir, env=f.env, progress=lines.append)
    assert proof is None or "fake: proving" in lines, lines
    return proof


@with_fixture("", jobs="8")
def test_a_proof_that_holds_passes_with_the_build_s_cap(f: Fixture) -> None:
    proof = _prove(f, _report("pass", must_run=2))
    assert proof is not None and proof.passed and proof.reasons == [], proof
    assert f.calls() == ["ensure --quiet", f"proof {f.dir} --json --delta -j 8"], f.calls()
    assert not f.session.warnings


@with_fixture("")
def test_a_failing_must_run_test_is_a_proof_that_does_not_hold_not_a_fallback(f: Fixture) -> None:
    why = "b: t0: fail (must run)"
    proof = _prove(f, _report("fail", reasons=[why], must_run=1))
    assert proof is not None and not proof.passed and proof.reasons == [why], proof
    assert not f.session.warnings, "a proof that does not hold is the gate's verdict, never a fallback"


@with_fixture("")
def test_tests_past_the_budget_do_not_block(f: Fixture) -> None:
    # The must-run tests ran past the budget, and ranked ones did not fit: the verdict is the client's, and it holds.
    proof = _prove(f, _report("pass", must_run=20, over_budget=5))
    assert proof is not None and proof.passed
    assert len(proof.report["plan"]["over_budget"]) == 5


def test_each_unavailable_code_from_proof_falls_back() -> None:
    for code in bs.UNAVAILABLE:
        with tempfile.TemporaryDirectory() as d:
            f = Fixture(Path(d), f"proof={code}")
            assert _prove(f, None) is None, code
            assert f.session.warnings and bs.UNAVAILABLE[code] in f.session.warnings[0], f.session.warnings


@with_fixture("proof=1")
def test_a_proof_without_a_verdict_falls_back(f: Fixture) -> None:
    assert _prove(f, None) is None
    assert "without a verdict" in f.session.warnings[0], f.session.warnings


@with_fixture("ensure=11")
def test_a_server_that_does_not_answer_falls_back_to_the_full_gate(f: Fixture) -> None:
    assert _prove(f, _report("pass")) is None
    assert f.calls() == ["ensure --quiet"], f.calls()


def test_a_missing_client_falls_back_to_the_full_gate() -> None:
    with tempfile.TemporaryDirectory() as d:
        build = Path(d)
        (build / "build.ninja").write_text("", encoding="utf-8")
        s = bs.session(bs.BuildServer(client=build / "absent.exe", ship_proof=True), flag=True, environ={})
        assert s is not None and s.proof(build, env={}) is None
        assert "proving without the build server" in s.warnings[0], s.warnings


@with_fixture("", ninja=False)
def test_an_unconfigured_build_dir_falls_back_to_the_full_gate(f: Fixture) -> None:
    assert _prove(f, _report("pass")) is None
    assert f.calls() == [], "the server is never asked"


def _ship_ctx(config: bs.BuildServer | None):
    def die(msg: str) -> None:
        raise SystemExit(msg)
    return types.SimpleNamespace(policy=types.SimpleNamespace(build_server=config), root=Path("."), die=die)


def test_ship_proves_only_when_asked_and_opted_in() -> None:
    from tools.dev.cmd import ship
    on = bs.BuildServer(client=Path(sys.executable), enabled=True)
    asked = bs.BuildServer(client=Path(sys.executable), enabled=True, ship_proof=True)
    saved = os.environ.pop(bs.ENV_ENABLE, None)
    try:
        assert ship._prover(_ship_ctx(on), None) is None, "off by default"
        assert ship._prover(_ship_ctx(asked), None) is not None, "the project's setting"
        assert ship._prover(_ship_ctx(asked), False) is None, "--no-proof wins"
        assert ship._prover(_ship_ctx(on), True) is not None, "--proof for one run"
        off = bs.BuildServer(client=Path(sys.executable), ship_proof=True)
        assert ship._prover(_ship_ctx(off), True) is None, "never without the server opted in"
        assert ship._prover(_ship_ctx(None), True) is None
    finally:
        if saved is not None:
            os.environ[bs.ENV_ENABLE] = saved


def test_ship_s_gate_takes_the_proof_for_the_build_and_test_steps() -> None:
    from tools.dev.cmd import ship
    calls: list[tuple] = []
    saved = (ship._dev, ship._dirty, ship._prove)
    ship._dev = lambda ctx, *argv: calls.append(argv) or 0
    ship._dirty = lambda repo: []
    ctx = _ship_ctx(None)
    try:
        for proven, expected in ((True, [("check", "--fix", "--no-test")]),
                                 (None, [("check", "--fix", "--no-test"), ("check", "test", "--memo")])):
            calls.clear()
            ship._prove = lambda ctx, server, p=proven: p
            ship._gate(ctx, tests=True, prover=object())
            assert calls == expected, (proven, calls)
        calls.clear()
        ship._prove = lambda ctx, server: False
        try:
            ship._gate(ctx, tests=True, prover=object())
            raise AssertionError("a proof that does not hold must stop ship")
        except SystemExit as e:
            assert "proof does not hold" in str(e), e
        assert calls == [("check", "--fix", "--no-test")], "and the full gate does not run after it"
        calls.clear()
        ship._gate(ctx, tests=True)
        assert calls[-1] == ("check", "test", "--memo"), "without a prover the gate is today's"
    finally:
        ship._dev, ship._dirty, ship._prove = saved


def test_a_red_run_at_the_change_s_base_decides_whether_it_blocks() -> None:
    from tools.dev.cmd import ship
    red = lambda t, judged, filed=None: {"binary": "b", "test": t, "outcome": "fail", "why": "must run",  # noqa: E731
                                         "judged": judged, **({"filed": filed} if filed else {})}
    ran = [red("tied", "tied"), red("untied", "untied", "nothing ties it"), red("flaky", "flaky", "flaky: green on a rerun"),
           red("unknown", "unknown"), {"binary": "b", "test": "ok", "outcome": "pass", "why": "must run"}]
    reasons = ["b: tied: fail (must run; tied)", "b: unknown: fail (must run; no evidence)", "the build is not green"]
    proof = bs.Proof(passed=False, reasons=reasons, report={"ran": ran, "delta": {"base": "abc123"}})
    asked: list[str] = []

    def at_base(ctx, preset, base, reds):
        asked.extend(r["test"] for r in reds)
        return {("b", "tied"): True, ("b", "untied"): False, ("b", "unknown"): None}

    saved = ship._red_at_base
    ship._red_at_base = at_base
    try:
        passed, filed, blockers = ship._settle_at_base(types.SimpleNamespace(), None, proof)
    finally:
        ship._red_at_base = saved
    assert asked == ["tied", "untied", "unknown"], "flaky is the server's to settle; the rest run at the base"
    assert [f[1] for f in filed] == ["tied", "flaky"], filed
    assert "red at the change's base" in filed[0][3]
    assert any(b.startswith("b: untied: fail (green at the change's base") for b in blockers), blockers
    assert any(b == "b: unknown: fail (must run)" for b in blockers), "no base result: the server's judgement"
    assert "the build is not green" in blockers and not passed


def _reds_at_base(reds: list[dict], cases: dict[str, dict[str, dict[str, bool]]], *, crashes=(), unbuilt=()):
    """`ship._red_at_base` over a fake worktree: `cases[at][binary]` is each case red or not at `base` and `head`,
    a binary in `crashes` ends non-zero without a report there (`(at, binary)`), one in `unbuilt` fails to build at the base.
    Returns the dev.py commands it ran, as (at, verb, binary, the case it named or None), and its answer."""
    from tools.dev.cmd import ship
    from tools.dev.lib.project import base_worktree

    with tempfile.TemporaryDirectory() as tmp:
        root, wt = Path(tmp) / "co", Path(tmp) / "wt"
        binaries = sorted({r["binary"] for r in reds})
        art = lambda b: root / "build" / "bin" / f"{b}.exe"  # noqa: E731
        ctx = types.SimpleNamespace(root=root, discover=lambda p: [types.SimpleNamespace(name=b, artifact=str(art(b))) for b in binaries])
        preset = types.SimpleNamespace(name="p", build_dir=root / "build")
        (wt / "build" / "bin").mkdir(parents=True)
        state, runs = {"at": "base"}, []

        def ensure(_root, commit):
            state["at"] = "head" if commit == "h" * 40 else "base"
            return wt, ""

        def run(cmd, **_kw):
            if cmd[0] == "git":
                return types.SimpleNamespace(stdout="h" * 40 + "\n", returncode=0)
            verb, rest = cmd[3], cmd[4:cmd.index("--preset")]
            binary = rest[rest.index("-t") + 1]
            only = [a for a in rest[rest.index("-t") + 2:] if a != "--no-build"]
            at = state["at"]
            runs.append((at, verb, binary, only[0] if only else None))
            if verb == "build":
                return types.SimpleNamespace(returncode=1 if (at, binary) in unbuilt else 0)
            if (at, binary) in crashes:
                return types.SimpleNamespace(returncode=3)
            # as nexus selects: by the test's own name, which a case of an invocable test carries last in its path
            mine = {t: red for t, red in cases[at][binary].items() if not only or t.rsplit(" / ", 1)[-1] in only}
            body = "".join(f'<testcase name="{t}">{"<failure/>" if red else ""}</testcase>' for t, red in mine.items())
            (wt / "build" / "bin" / f"{binary}.exe.results.xml").write_text(f"<testsuites><testsuite>{body}</testsuite></testsuites>")
            return types.SimpleNamespace(returncode=1 if any(mine.values()) else 0)

        saved = base_worktree.ensure, ship.subprocess, ship._say
        base_worktree.ensure, ship.subprocess, ship._say = ensure, types.SimpleNamespace(run=run, DEVNULL=-3), lambda *_: None
        try:
            out = ship._red_at_base(ctx, preset, "base", reds)
        finally:
            base_worktree.ensure, ship.subprocess, ship._say = saved
    return [r for r in runs if r[1] == "test"], out


def test_reds_at_the_base_run_each_binary_once() -> None:
    # The base has x and z red and y green; at the commit being pushed x passes, as a flaky test may.
    # A red at the base is the base's whatever the change's own run shows, so nothing runs at HEAD.
    cases = {"base": {"b": {"x": True, "y": False, "z": True}, "c": {"w": False}},
             "head": {"b": {"x": False, "y": True, "z": True}, "c": {"w": True}}}
    reds = [{"binary": "b", "test": t, "outcome": "fail"} for t in "xyz"] + [{"binary": "c", "test": "w", "outcome": "fail"}]
    runs, out = _reds_at_base(reds, cases)
    assert runs == [("base", "test", "b", None), ("base", "test", "c", "w")], \
        f"several reds of a binary run it whole, once; a lone red runs alone; nothing runs at HEAD: {runs}"
    assert out == {("b", "x"): True, ("b", "y"): False, ("b", "z"): True, ("c", "w"): False}, out


def test_a_lone_red_of_an_invocable_test_runs_by_its_own_name() -> None:
    # a case named by its path selects nothing as a filter, so the base run selects the test and reads its case back
    cases = {"base": {"v": {"drv / one / t": True, "drv / two / t": False}}, "head": {}}
    runs, out = _reds_at_base([{"binary": "v", "test": "drv / one / t", "outcome": "fail"}], cases)
    assert runs == [("base", "test", "v", "t")], runs
    assert out == {("v", "drv / one / t"): True}, out


def test_a_binary_that_ends_without_a_report_at_the_base_too_is_red_there() -> None:
    # d crashed in the checkout, every case red by its ending; at the base it crashes too.
    # e crashed only with the change: its base run reports, and its cases are green there.
    # f cannot be built at the base, which says nothing; g's own failing case at a crashing base is no result either.
    cases = {"base": {"e": {"p": False, "q": False}, "g": {}}, "head": {}}
    crashed = lambda b, t: {"binary": b, "test": t, "outcome": "failed"}  # noqa: E731
    reds = [crashed("d", "p"), crashed("d", "q"), crashed("e", "p"), crashed("e", "q"), crashed("f", "p"),
            {"binary": "g", "test": "p", "outcome": "fail"}]
    runs, out = _reds_at_base(reds, cases, crashes={("base", "d"), ("head", "d"), ("base", "g")}, unbuilt={("base", "f")})
    assert ("base", "test", "d", None) in runs and not any(r[0] == "head" for r in runs), "a crashed binary runs whole, at the base alone"
    assert ("base", "test", "e", None) in runs and not any(r[2] == "f" for r in runs), runs
    assert out == {("d", "p"): True, ("d", "q"): True, ("e", "p"): False, ("e", "q"): False, ("f", "p"): None, ("g", "p"): None}, out


def test_a_crash_at_the_base_too_is_filed_as_unproven_and_listed_once() -> None:
    from tools.dev.cmd import ship
    ran = [{"binary": "d", "test": f"t{i}", "outcome": "failed", "why": "must run", "judged": "unknown"} for i in range(30)]
    proof = bs.Proof(passed=False, reasons=[f"d: t{i}: failed (must run)" for i in range(30)], report={"ran": ran, "delta": {"base": "abc123"}})
    saved = ship._red_at_base
    ship._red_at_base = lambda ctx, preset, base, reds: {(r["binary"], r["test"]): True for r in reds}
    try:
        passed, filed, blockers = ship._settle_at_base(types.SimpleNamespace(), None, proof)
    finally:
        ship._red_at_base = saved
    assert passed and not blockers, blockers
    assert len(filed) == 30 and all("ends without a report at the change's base" in f[3] for f in filed), filed[:2]


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

