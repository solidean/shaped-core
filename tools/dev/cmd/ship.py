"""`ship` — pull, gate and push this repository and every submodule carrying unpushed work, in the one order that is safe.

A change that spans submodules lands as: each submodule's commits on its branch, the parent's gate over all of it,
the submodules pushed, the pointers bumped in a commit of their own, the parent pushed.
Done by hand, that sequence goes wrong in the obvious places, a submodule pushed before the gate ran or from the wrong directory,
so `ship` is the only command that pushes.

The gate is the project's own `dev.py check`, whatever registry the project wired, so its extra roots already cover the submodules' prose and links.
A push the remote rejected because it moved merges again and re-runs the static gates on top of what came in, up to three rounds;
the tests ran on this change already, and what came in was gated by whoever pushed it.
A submodule's push is retried the same way, its own static gate re-run and its pointer bumped again, within the same rounds.

The gate builds and tests through the build server when it is opted in, as `check` does; `--no-server` keeps every gate off it.
With the build server opted in, `--proof` (or the project's `BuildServer.ship_proof`) replaces the gate's build and test steps with the server's proof
of the change being pushed; a red blocks only when the change turned it red, and the others are listed to be filed:
a green build of exactly this tree and every test green at least once, the tests without such a result run, then the likeliest affected within a short budget.
The static gates run as always; any problem with the server runs the full gate instead (lib/pipeline/build_server.py).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools import dev
from tools.dev import console

from ..lib.pipeline import build_server
from ..lib.pipeline import test as pipeline_test
from ..lib.project import submodules as lib
from . import args as a
from .context import Context

NAME = "ship"

# Rounds before a busy remote wins: each one pulls what came in and gates again.
_ROUNDS = 3


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Pull, gate and push this repository and every submodule with unpushed work, bumps included")
    p.add_argument("--bump", action="append", default=[], metavar="PATH",
                   help="Also bump this submodule, whose work is pushed already but whose pointer is not; repeatable")
    p.add_argument("--closes", metavar="ID[,ID]", default=None,
                   help="Close these issues in a commit of their own ahead of the gate, so the push that resolves them closes them; "
                        "nothing closes if the gate fails")
    p.add_argument("--resolution", choices=("fixed", "wontfix", "duplicate", "obsolete"), default="fixed",
                   help="How --closes closes them (default: fixed)")
    p.add_argument("--dry-run", action="store_true", help="Say what would be pulled, bumped and pushed, and change nothing")
    p.add_argument("--proof", action=argparse.BooleanOptionalAction, default=None,
                   help="Prove the build and the tests through the build server instead of the gate's build and test steps "
                        "(default: the project's BuildServer.ship_proof, off); needs the server opted in, "
                        "and runs the full gate whenever the server cannot prove")
    a.server(p)
    return p


def _git(repo: Path, *argv: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *argv], capture_output=True, text=True, encoding="utf-8", errors="replace")


def _say(text: str) -> None:
    dev.ui.write_line(console.dim(f"ship: {text}"))


def _dev(ctx: Context, *argv: str) -> int:
    """Run this project's own dev.py, so the project's registry and policy apply, not shaped-core's defaults."""
    return subprocess.run(["uv", "run", "dev.py", *argv], cwd=str(ctx.root)).returncode


def _branch_of(repo: Path) -> str | None:
    head = _git(repo, "symbolic-ref", "--quiet", "--short", "HEAD")
    return head.stdout.strip() if head.returncode == 0 else None


def _unpushed(repo: Path, branch: str) -> int:
    """Commits on HEAD that the remote's branch lacks; every commit when the branch was never pushed."""
    if _git(repo, "rev-parse", "--verify", "--quiet", f"origin/{branch}").returncode != 0:
        return int(_git(repo, "rev-list", "--count", "HEAD").stdout.strip() or "0")
    return int(_git(repo, "rev-list", "--count", f"origin/{branch}..HEAD").stdout.strip() or "0")


def _dirty(repo: Path) -> list[str]:
    """What is uncommitted in `repo`, a submodule's moved pointer aside: that is a bump, not an edit."""
    out = _git(repo, "status", "--porcelain", "--untracked-files=normal", "--ignore-submodules=all")
    return [line for line in out.stdout.splitlines() if line.strip()]


def _changed_during_gate(dirty: list[str], advice: str, whose: str = "the ") -> str:
    """Files uncommitted after the gate: its own fixes, or edits made while it ran, which it cannot tell apart."""
    files = "\n".join(f"  {line}" for line in dirty)
    return f"ship: files are uncommitted after {whose}gate, rewritten by its fixes or edited while it ran; nothing was pushed:\n{files}\n{advice}"


def _merge_upstream(ctx: Context, repo: Path, branch: str, label: str) -> None:
    fetched = _git(repo, "fetch", "origin", branch)
    if fetched.returncode != 0 and "couldn't find remote ref" not in fetched.stderr:
        ctx.die(f"ship: git fetch failed in {label}:\n{fetched.stderr.strip()}")
    if _git(repo, "rev-parse", "--verify", "--quiet", f"origin/{branch}").returncode != 0:
        return
    if _git(repo, "merge-base", "--is-ancestor", f"origin/{branch}", "HEAD").returncode == 0:
        return
    _say(f"merging origin/{branch} into {label}")
    merged = _git(repo, "merge", "--no-edit", f"origin/{branch}")
    if merged.returncode != 0:
        ctx.die(f"ship: the merge in {label} did not go through; resolve it, commit, and ship again:\n{(merged.stdout + merged.stderr).strip()}")


def _commit_closes(ctx: Context, ids: list[str], resolution: str, subs: list[lib.Submodule]) -> None:
    """Close each issue and commit the closes as one commit on top of HEAD, ahead of the gate, which validates them like any other.
    A moved submodule pointer stays out of it: a bump is a commit of its own."""
    for issue_id in ids:
        if _dev(ctx, "issues", "close", issue_id, "--resolution", resolution) != 0:
            ctx.die(f"ship: could not close {issue_id}")
    _git(ctx.root, "add", "-A")
    if subs:
        _git(ctx.root, "reset", "-q", "--", *[s.path for s in subs])
    subject = f"issues: close {', '.join(ids)} ({resolution})"
    committed = _git(ctx.root, "commit", "-q", "-m", subject, "-m", "Resolved by the commits pushed with this one.")
    if committed.returncode != 0:
        ctx.die(f"ship: git commit failed after closing the issues:\n{(committed.stdout + committed.stderr).strip()}")
    _say(f"committed `{subject}`")


def _bump(ctx: Context, subs: list[lib.Submodule]) -> None:
    """Commit the moved pointers as one commit of their own, the presets the project derives from a submodule refreshed with them."""
    moved = [s for s in subs if lib.recorded(ctx.root, s) != _git(ctx.root / s.path, "rev-parse", "HEAD").stdout.strip()]
    if not moved:
        return
    paths = [s.path for s in moved]
    # A project whose registry derives a file from a submodule (CMakePresets.json from shaped-core's) refreshes it in the same commit.
    listed = subprocess.run(["uv", "run", "dev.py", "check", "--list"], cwd=str(ctx.root), capture_output=True, text=True)
    if "presets" in listed.stdout.split():
        _dev(ctx, "check", "presets", "--fix")
        paths += [line[3:] for line in _dirty(ctx.root)]
    _git(ctx.root, "add", "--", *paths)
    names = ", ".join(Path(s.path).name for s in moved)
    committed = _git(ctx.root, "commit", "-q", "-m", f"bump {names}", "--", *paths)
    if committed.returncode != 0:
        ctx.die(f"ship: the bump commit failed:\n{(committed.stdout + committed.stderr).strip()}")
    _say(f"committed the bump of {names}")


def _on_remote(repo: Path, sha: str) -> bool:
    """Whether a remote branch of `repo` holds `sha`, after fetching every branch once if the first look misses it."""
    def held() -> bool:
        found = _git(repo, "branch", "-r", "--contains", sha)
        return found.returncode == 0 and found.stdout.strip() != ""
    if held():
        return True
    _git(repo, "fetch", "origin")
    return held()


def _pointers_off_remote(ctx: Context, branch: str, subs: list[lib.Submodule]) -> list[str]:
    """Submodule pointers the commits about to be pushed record that no remote branch of their submodule holds.
    Pushed, such a pointer breaks every clone and pull that recurses into submodules, so ship refuses it."""
    base = f"origin/{branch}"
    span = f"{base}..HEAD" if _git(ctx.root, "rev-parse", "--verify", "--quiet", base).returncode == 0 else "HEAD"
    missing = []
    for s in subs:
        commits = _git(ctx.root, "log", "--format=%H", span, "--", s.path).stdout.split()
        shas = []
        for c in commits:
            entry = _git(ctx.root, "ls-tree", c, "--", s.path).stdout.split()
            if len(entry) >= 3 and entry[1] == "commit" and entry[2] not in shas:
                shas.append(entry[2])
        for sha in shas:
            if not _on_remote(ctx.root / s.path, sha):
                missing.append(f"{s.path} at {sha[:12]}")
    return missing


def _prover(ctx: Context, flag: bool | None) -> build_server.Session | None:
    """The build server to prove through, or None for the full gate: `--proof`/`--no-proof`, else the project's `ship_proof`.

    Only with the server opted in, as `build` and `test` decide it; a missing client still gives a Session, which warns and falls back.
    """
    config = ctx.policy.build_server
    if not (flag if flag is not None else bool(config and config.ship_proof)):
        return None
    session = build_server.session(config, flag=None)
    if session is None:
        _say("--proof: the build server is not opted in here, so the full gate runs")
    return session


def _prove(ctx: Context, server: build_server.Session) -> bool | None:
    """The server's proof of the default preset's build: whether it holds, or None when the full gate has to run instead."""
    preset = ctx.resolve_presets([ctx.default_preset_name()])[0]
    # Built here first, in the build environment and through the server: the proof's own build runs in the tests'
    # environment, which its results are keyed on, and a tool such as rc.exe cannot build in that one.
    # `all` and every test binary, as the proof builds them, since a project may leave one out of `all`.
    targets = ["all", *(t.name for t in ctx.discover(preset) if ctx.is_test_target(t))]
    results = dev.build([preset], targets, root=ctx.root, auto_configure=True, server=server)
    if not all(r.ok for r in results):
        dev.report.print_build_failure(results, [preset], ctx.root)
        return False
    proof = server.proof(preset.build_dir, env=pipeline_test.server_env(preset, ctx.root),
                         progress=lambda line: dev.ui.write_line(console.dim(f"  {line}")))
    if proof is None:
        return None
    plan = proof.report.get("plan") or {}
    ran = proof.report.get("ran") or []
    _say(f"proof: {proof.report.get('declared', 0)} test(s), {len(plan.get('must_run') or [])} without a green result ran, "
         f"{len([r for r in ran if r.get('why') == 'affected'])} likely affected ran, "
         f"{len(plan.get('over_budget') or [])} past the budget")
    for s in proof.report.get("sideband") or []:
        _say(f"sideband, not blocking: {s.get('config')} {s.get('mode')} {s.get('binary')}: {s.get('outcome')} {s.get('range')}")
    passed, filed, blockers = _settle_at_base(ctx, preset, proof)
    # Reds the change did not cause: flaky, red before it, or red at its base too.
    # The push goes on; each is the session's to file as an issue, after `issues known <test>`.
    # Past a handful of one binary and reason (a binary that crashes has every case red), they are one line.
    groups: dict[tuple[str, str, str], list[str]] = {}
    for binary, test, outcome, why in filed:
        groups.setdefault((binary, outcome, why), []).append(test)
    for (binary, outcome, why), tests in groups.items():
        named = tests[0] if len(tests) == 1 else f"{len(tests)} tests" if len(tests) > 5 else ", ".join(tests)
        dev.ui.write_line(console.yellow(f"ship: not blocking, file an issue: {binary}: {named}: {outcome} ({why})"))
    if filed:
        _say(f"{len(filed)} red test(s) above are not this change's; `uv run dev.py issues known <test>`, then file each that is not known")
    for reason in blockers:
        dev.ui.write_line(console.red(f"ship: proof: {reason}"))
    return passed


# The judgements only a run at the change's base settles; the server decides `flaky` and `red_before` itself.
_SETTLED_AT_BASE = {"green_before", "tied", "untied", "unknown"}


def _settle_at_base(ctx: Context, preset, proof: build_server.Proof) -> tuple[bool, list[tuple[str, str, str, str]], list[str]]:
    """The proof's verdict once its reds are run at the change's base: whether it holds, the reds to file, the blockers.

    A red the server judged by its history or its evidence alone is run again in a worktree at the base (lib/project/base_worktree.py):
    red there too, the change did not cause it, whatever the evidence said; green there, the change did, and it blocks.
    A red whose base run cannot be made or read keeps the server's judgement.
    """
    report = proof.report
    ran = report.get("ran") or []
    red = [r for r in ran if r.get("outcome") not in ("pass", "skip")]
    unfiled = [r for r in red if not r.get("filed")]
    # Each red that blocks is one of the proof's reasons; the rest (a failed build, a moved tree, an unbuilt binary) stand.
    others = [why for why in proof.reasons if not any(why.startswith(f"{r['binary']}: {r['test']}: ") for r in unfiled)]
    settle = [r for r in red if r.get("judged") in _SETTLED_AT_BASE]
    at_base: dict[tuple[str, str], bool | None] = {}
    base = (report.get("delta") or {}).get("base")
    if settle and base:
        at_base = _red_at_base(ctx, preset, base, settle)
    filed, blockers = [], list(others)
    for r in red:
        key, desc = (r["binary"], r["test"]), f"{r['binary']}: {r['test']}: {r['outcome']}"
        verdict = at_base.get(key)
        if verdict is True and _binary_red(r):
            filed.append((*key, r["outcome"], f"its binary ends without a report at the change's base {base[:12]} too, so the case is unproven"))
        elif verdict is True:
            filed.append((*key, r["outcome"], f"red at the change's base {base[:12]} too"))
        elif verdict is False:
            blockers.append(f"{desc} (green at the change's base {base[:12]}, red with it)")
        elif r.get("filed"):
            filed.append((*key, r["outcome"], r["filed"]))
        else:
            blockers.append(f"{desc} ({r.get('why')})")
    return not blockers, filed, blockers


def _red_at_base(ctx: Context, preset, base: str, reds: list[dict]) -> dict[tuple[str, str], bool | None]:
    """Per red (binary, test): whether it is red at `base`, run through `dev.py test` in the base worktree; None where unknown."""
    from ..lib.project import base_worktree

    _say(f"running {len(reds)} red test(s) at the change's base {base[:12]}, in {base_worktree.DIR}")
    wt, why = base_worktree.ensure(ctx.root, base)
    if wt is None:
        _say(f"the base worktree could not be made, so the server's judgement stands: {why}")
        return {}
    artifacts = {t.name: t.artifact for t in ctx.discover(preset) if t.artifact}
    try:
        build_rel = preset.build_dir.resolve().relative_to(ctx.root.resolve())
    except ValueError:
        _say(f"{preset.build_dir} is outside the checkout, so the base worktree has no build dir to mirror it")
        return {}
    def dev(*args: str) -> int:
        return subprocess.run(["uv", "run", "dev.py", *args, "--preset", preset.name], cwd=str(wt),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode

    def run(rs: list[dict]) -> dict[tuple[str, str], bool | None]:
        # One run per binary: a lone red of its own runs alone, anything more runs the whole binary, read from its one report,
        # since a run per case cost seconds each.
        by_binary: dict[str, list[dict]] = {}
        for r in rs:
            by_binary.setdefault(r["binary"], []).append(r)
        out: dict[tuple[str, str], bool | None] = {}
        for binary, mine in by_binary.items():
            keys = [(binary, r["test"]) for r in mine]
            art = artifacts.get(binary)
            # Built on its own first, so a binary that cannot be built at the base reads as no result, never as a crash there.
            if art is None or dev("build", "-t", binary) != 0:
                out.update(dict.fromkeys(keys))
                continue
            rel = Path(art).resolve().relative_to(preset.build_dir.resolve())
            xml = wt / build_rel / rel.parent / f"{rel.name}.results.xml"
            xml.unlink(missing_ok=True)
            # A binary that ended without a report (a crash, a timeout) has every case red by the binary's ending:
            # it says nothing of a single case, only the whole binary run again can show whether it ends that way at the base too.
            whole = len(mine) > 1 or any(_binary_red(r) for r in mine)
            # a case of an invocable test is named by its path, `driver / child / test`, which selects nothing as a filter;
            # its leaf selects every child, and the report names each by its path again
            rc = dev("test", "-t", binary, "--no-build", *([] if whole else [mine[0]["test"].rsplit(" / ", 1)[-1]]))
            found = _cases_red(xml)
            # a binary that wrote no report of its own gets dev.py's stand-in, one case named after the binary
            if set(found) == {binary}:
                found = {}
            ended_without_report = rc != 0 and not found
            for r, key in zip(mine, keys):
                out[key] = True if _binary_red(r) and ended_without_report else found.get(r["test"])
        return out

    out = run(reds)
    _say_at_base(out)
    # A red at the base is the base's, whatever a run at the change would show: a flaky test is red at the base too, and
    # a run of it at the change that happens to pass says nothing about the change.
    return out


def _say_at_base(out: dict[tuple[str, str], bool | None], listed: int = 10) -> None:
    """Each red's result at the base; past `listed` of one binary, its counts instead."""
    word = {True: "red", False: "green", None: "no result"}
    by_binary: dict[str, list[tuple[str, bool | None]]] = {}
    for (b, t), red in out.items():
        by_binary.setdefault(b, []).append((t, red))
    for b, rs in by_binary.items():
        if len(rs) <= listed:
            for t, red in rs:
                _say(f"  at the base: {b}: {t}: {word[red]}")
        else:
            counts = ", ".join(f"{sum(1 for _, r in rs if r is v)} {word[v]}" for v in (True, False, None))
            _say(f"  at the base: {b}: {len(rs)} red tests: {counts}")


def _binary_red(r: dict) -> bool:
    """Whether a red is its binary's ending (the server's `failed`, `timed out`) rather than the case's own `fail` in a report."""
    return r.get("outcome") in ("failed", "timed out")


def _cases_red(xml: Path) -> dict[str, bool]:
    """Each case of a JUnit report: failed (True) or passed (False); a skipped case and an unreadable report are absent, saying nothing."""
    import xml.etree.ElementTree as ET

    try:
        cases = list(ET.parse(xml).getroot().iter("testcase"))
    except (OSError, ET.ParseError):
        return {}
    out: dict[str, bool] = {}
    for c in cases:
        name = c.get("name")
        if name is None or c.find("skipped") is not None:
            continue
        # A name repeated (a dispatched child's) is red when any of its runs is.
        out[name] = out.get(name, False) or c.find("failure") is not None or c.find("error") is not None
    return out


def _gate(ctx: Context, *, tests: bool, prover: build_server.Session | None = None) -> None:
    # The static gates first, then the question only they raise, so a fixer's rewrite stops ship before the test tail runs.
    if _dev(ctx, "check", "--fix", "--no-test") != 0:
        ctx.die("ship: the gate failed; nothing was pushed (the bump commit, if any, stays local)")
    if dirty := _dirty(ctx.root):
        ctx.die(_changed_during_gate(dirty, "commit them and ship again"))
    if not tests:
        return
    # The server's proof stands in for the build and test steps; a server that cannot prove leaves them to run.
    proven = _prove(ctx, prover) if prover is not None else None
    if proven is False:
        ctx.die("ship: the proof does not hold; nothing was pushed (the bump commit, if any, stays local)")
    # --memo, so a test binary that already passed on these exact bytes is not run again.
    if proven is None and _dev(ctx, "check", "test", "--memo") != 0:
        ctx.die("ship: the tests failed; nothing was pushed (the bump commit, if any, stays local)")


def _gate_submodule(ctx: Context, s: lib.Submodule, *, tests: bool) -> None:
    """A submodule's own check, where it has a driver of its own: the static gates, then the tests where asked."""
    repo = ctx.root / s.path
    if not (repo / "dev.py").is_file():
        return
    _say(f"gating {s.path} with its own check")
    if subprocess.run(["uv", "run", "dev.py", "check", "--fix", "--no-test"], cwd=str(repo)).returncode != 0:
        ctx.die(f"ship: {s.path}'s own gate failed; nothing was pushed")
    if dirty := _dirty(repo):
        ctx.die(_changed_during_gate(dirty, f"commit them in {s.path} and ship again", f"{s.path}'s "))
    if tests and subprocess.run(["uv", "run", "dev.py", "check", "test"], cwd=str(repo)).returncode != 0:
        ctx.die(f"ship: {s.path}'s own tests failed; nothing was pushed")


def _is_rejected(text: str) -> bool:
    """Whether a failed push failed because the remote moved, which a merge answers, rather than for a reason it does not."""
    return "fetch first" in text or "non-fast-forward" in text or "rejected" in text


def run(args: argparse.Namespace, ctx: Context) -> None:
    # The gates run in child dev.py processes, the submodules' own among them, and read the flags from the environment.
    build_server.export(a.server_flag(args))
    branch = _branch_of(ctx.root)
    if branch is None:
        ctx.die("ship needs a checked-out branch here")
    if dirty := _dirty(ctx.root):
        ctx.die("ship needs every edit committed; uncommitted here:\n  " + "\n  ".join(dirty[:10]))

    # The submodules that carry work of their own: unpushed commits on the branch .gitmodules names.
    subs = lib.list_submodules(ctx.root)
    named = {Path(p).as_posix().rstrip("/") for p in args.bump}
    shipping: list[lib.Submodule] = []
    bumping: list[lib.Submodule] = []
    for s in subs:
        repo = ctx.root / s.path
        if dirty := _dirty(repo):
            ctx.die(f"ship needs every edit committed; uncommitted in {s.path}:\n  " + "\n  ".join(dirty[:10]))
        if s.branch is None:
            continue
        _git(repo, "fetch", "origin", s.branch)
        ahead = _unpushed(repo, s.branch) if _branch_of(repo) == s.branch else 0
        if _branch_of(repo) != s.branch and _git(repo, "branch", "--contains", "HEAD", "-r").stdout.strip() == "":
            ctx.die(f"ship: {s.path} holds commits on no branch; check out {s.branch} there first")
        if ahead:
            shipping.append(s)
        if ahead or s.path in named:
            bumping.append(s)
    unknown = named - {s.path for s in subs}
    if unknown:
        ctx.die(f"ship --bump: not a submodule: {', '.join(sorted(unknown))}")

    _say(f"submodules with unpushed work: {', '.join(s.path for s in shipping) or 'none'}")
    prover = _prover(ctx, args.proof)
    if args.dry_run:
        _say(f"would bump: {', '.join(s.path for s in bumping) or 'nothing'}; would push {branch} and those submodules")
        _say("would prove the build and the tests through the build server" if prover is not None
             else "would run the full gate's build and test steps")
        return

    for s in shipping:
        _merge_upstream(ctx, ctx.root / s.path, s.branch, s.path)
    _merge_upstream(ctx, ctx.root, branch, "this repository")
    _dev(ctx, "submodules", "sync")  # brings the others to what a pull recorded; never touches the ones ahead

    # A submodule with a driver of its own has a gate of its own, which its rules require before its push; the parent's gate does not run it.
    for s in shipping:
        _gate_submodule(ctx, s, tests=True)

    _bump(ctx, bumping)
    if args.closes:
        _commit_closes(ctx, [i.strip() for i in args.closes.split(",") if i.strip()], args.resolution, subs)

    for round_no in range(1, _ROUNDS + 1):
        # A later round follows a merge the remote forced: the static gates see what it broke, and the tests ran on this change already.
        _gate(ctx, tests=round_no == 1, prover=prover)
        # Submodules first, so the pointers the parent pushes always name commits their remotes have.
        # One whose remote moved merges what came in, gates it statically and is bumped again, and the round starts over.
        moved = None
        for s in list(shipping):
            pushed = _git(ctx.root / s.path, "push", "origin", f"HEAD:{s.branch}")
            if pushed.returncode != 0:
                if not _is_rejected(pushed.stdout + pushed.stderr):
                    ctx.die(f"ship: pushing {s.path} failed:\n{pushed.stderr.strip()}")
                moved = s
                break
            _say(f"pushed {s.path} to origin/{s.branch}")
            shipping.remove(s)
        if moved is not None:
            _say(f"origin/{moved.branch} of {moved.path} moved during the gate (round {round_no}); merging it and gating again")
            _merge_upstream(ctx, ctx.root / moved.path, moved.branch, moved.path)
            _gate_submodule(ctx, moved, tests=False)
            _bump(ctx, [moved])
            continue
        # Every pointer the parent's new commits record has to be fetchable, or the push breaks the next recursive pull.
        if missing := _pointers_off_remote(ctx, branch, subs):
            ctx.die("ship: these submodule pointers name commits no remote branch holds; push them, or bump to ones it does, "
                    "and ship again (nothing of this repository was pushed):\n  " + "\n  ".join(missing))
        pushed = _git(ctx.root, "push", "origin", branch)
        if pushed.returncode == 0:
            dev.ui.write_line(console.green(f"ship: pushed {branch} to origin"))
            return
        text = pushed.stdout + pushed.stderr
        if not _is_rejected(text):
            ctx.die(f"ship: git push failed:\n{text.strip()}")
        _say(f"origin/{branch} moved during the gate (round {round_no}); merging it and gating again")
        _merge_upstream(ctx, ctx.root, branch, "this repository")
        _dev(ctx, "submodules", "sync")
    ctx.die(f"ship: a remote moved {_ROUNDS} times while gating; it is busy, ship again in a moment")
