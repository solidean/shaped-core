"""`example` — list the repo's examples, or build and run exactly one.

An example is a nexus `EXAMPLE` declaration: a runnable demonstration of an API in practice, in its own selection bucket.
Every build compiles them and nothing runs them automatically, so this is how one gets executed.

Resolution is a cross-binary name lookup, not a target lookup: an example name says nothing about the binary carrying it, so every `*-example` target is built and probed.
`--target` narrows which binaries are probed; the match never does.
docs/guides/examples.md is the concept behind all of it.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.pipeline.examples import (capture_directory, collect_examples, is_example_target,
                                             resolve_example, select_examples)

from . import args as a
from .context import Context

NAME = "example"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run one example, or list them all (args after the match are forwarded)")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    a.profile(p)
    p.add_argument("--target", "-t", action="append",
                   help="Example binary target(s) to consider: comma-list, repeatable, wildcards")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--timeout", type=float, default=0.0, metavar="SECS",
                   help="Kill the example after SECS (default: 0, no limit — an example may be interactive)")
    p.add_argument("--background", action="store_true",
                   help="Ask the example not to steal focus: its windows appear without being activated. "
                        "Sets SC_REQUEST_BACKGROUND=1, which sr::window_system reads (sr::background_request_env_var).")
    p.add_argument("--capture", nargs="?", const="", metavar="NAME",
                   help="Run headless and write an image instead of opening a window: no display is needed. "
                        "With NAME, take the capture the example registered under that name. "
                        "Sets SC_CAPTURE, which sv::interactive reads (libs/graphics/shaped-viewer/src/shaped-viewer/capture.hh).")
    p.add_argument("--capture-out", metavar="PATH",
                   help="Where --capture writes. Defaults to build/<preset>/captures/<name>/<shot>/capture.jpg; "
                        "the extension picks the format.")
    p.add_argument("--capture-size", metavar="WxH", default="1920x1080",
                   help="Resolution --capture renders at (default: 1920x1080)")
    p.add_argument("--capture-accumulate", metavar="N", type=int,
                   help="Accumulated frames every traced view must reach before --capture writes (default: 60)")
    p.add_argument("--capture-timeout", metavar="SECS", type=float,
                   help="How long --capture may spend before it gives up and writes what it has (default: 60)")
    p.add_argument("--capture-list", action="store_true",
                   help="Print the capture names the example registers, then exit. Runs one headless frame to find out.")
    p.add_argument("--update-captures", nargs="?", const="", metavar="MATCH",
                   help="Capture every example MATCH selects (each of its registered shots), then copy the successful "
                        "ones next to their source. Omit MATCH for the whole corpus. Headless, so unlike running "
                        "examples this opens no windows.")
    p.add_argument("--refresh-captures", nargs="?", const="", metavar="MATCH",
                   help="Copy already-captured images next to their examples, without re-capturing. Takes its own "
                        "MATCH, so a sweep over everything can be refreshed for a subset.")
    p.add_argument("--test-args", metavar="LINE",
                   help="A command line for the example itself, reachable from its body through "
                        "nx::test_args(). Forwarded as one string and tokenized by the runner, so the "
                        "example's own flags never collide with dev.py's. Replaces whatever the example "
                        "declared with nx::config::args.")
    p.add_argument("match", nargs="?",
                   help="Example name, or a substring of one. Must select exactly one; omit to list them all.")
    # Everything dev.py does not recognize is forwarded verbatim to the example, collected into args.runner_args.
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    preset = presets[0]

    if args.refresh_captures is not None and args.update_captures is None:
        _refresh(ctx, preset, args.refresh_captures)
        return

    program_args = list(args.runner_args or [])
    if program_args and program_args[0] == "--":
        program_args = program_args[1:]

    # One string, tokenized by the runner, so the example's own flags never have to survive dev.py's
    # argument handling — and never collide with the --examples the launch below already passes.
    if args.test_args is not None:
        program_args = ["--test-args", args.test_args, *program_args]

    wanted = ctx.resolve_target_names(preset, args.target, args.emsdk_path) if args.target else None

    # Every example binary is built before probing: a listing can only come from an artifact that exists.
    # Incremental, so this is cheap once the corpus is built, and it is also what keeps a stale name from resolving.
    if not args.no_build:
        targets = ctx.discover(preset, args.emsdk_path)
        names = wanted if wanted is not None else [t.name for t in targets if is_example_target(t)]
        if not names:
            ctx.die("no *-example targets are configured. Is SC_BUILD_EXAMPLES on?")
        results = dev.build([preset], names, root=ctx.root,
                            auto_configure=not args.no_configure,
                            mirror=args.mirror_output, verbose=args.verbose,
                            emsdk_path=args.emsdk_path)
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])

    # Discovered after the build: a first-time configure knows the target but has not linked its artifact yet.
    targets = ctx.discover(preset, args.emsdk_path)
    examples = collect_examples(preset, targets, root=ctx.root, binary_names=wanted)

    if args.update_captures is not None:
        _sweep(ctx, preset, targets, examples, args)
        return

    if args.match is None:
        _print_listing(ctx, examples)
        return

    example, diagnostic = resolve_example(examples, args.match)
    if example is None:
        ctx.die(diagnostic)

    artifact = next((t.artifact for t in targets if t.name == example.target and t.artifact), None)
    if artifact is None:
        ctx.die(f"target {example.target!r} has no built artifact for preset {preset.name!r}")

    print(console.dim(f"{example.name}  ({example.target}, {example.file}:{example.line})"), file=sys.stderr)

    # An environment variable rather than a flag: the example binary is a program dev.py did not write, and nexus has no say over what a window system does.
    # sr::window_system reads this one; see docs/guides/examples.md.
    env = None
    if args.background:
        env = {**os.environ, "SC_REQUEST_BACKGROUND": "1"}

    capture_path = None
    if args.capture is not None or args.capture_list:
        capture_path, capture_env = _capture_environment(ctx, preset, example, args)
        env = {**(env or os.environ), **capture_env}

    # The exact name plus the bucket flag: an example is never swept, so it must be named to run.
    # Mirrored, because watching the example run is the entire point of the command.
    result = dev.run_step(
        [str(artifact), example.name, "--examples", *program_args],
        step_type="example", name=example.target,
        build_dir=preset.build_dir, cwd=_working_directory(ctx, example), env=env,
        timeout=args.timeout if args.timeout else None,
        mirror=True, verbose=args.verbose,
    )
    if capture_path is not None and result.returncode == 0:
        print(console.dim(f"capture -> {ctx.rel(capture_path)}"), file=sys.stderr)

    sys.exit(result.returncode)


# A sweep bounds each run: the interactive default of no timeout is right for a window somebody is watching and
# wrong for a corpus refresh, where one example that never settles would hang the whole thing.
_SWEEP_TIMEOUT_SECONDS = 300.0

# What sv prints each registered capture name behind; see sv::capture_list_prefix in
# libs/graphics/shaped-viewer/src/shaped-viewer/capture.hh.
_LIST_PREFIX = "sv-capture: "


def _sweep(ctx: Context, preset, targets, examples: list, args: argparse.Namespace) -> None:
    """Capture every example the matcher selects, then refresh the ones that worked.

    One process per shot, so every shot starts from an identical cold state — which is what makes a committed image
    reproducible rather than dependent on what ran before it in the same process.
    """
    selected = select_examples(examples, args.update_captures)
    if not selected:
        ctx.die(f"no example matches {args.update_captures!r}")

    by_target = {t.name: t for t in targets}
    report: list[tuple[str, str, str]] = []  # (example, shot, outcome)

    for example in selected:
        artifact = by_target.get(example.target)
        artifact = artifact.artifact if artifact else None
        if artifact is None:
            report.append((example.name, "-", "skipped: no built artifact"))
            continue

        shots = [""] + _registered_shots(ctx, preset, example, artifact, args)
        for shot in shots:
            outcome = _capture_one(ctx, preset, example, artifact, shot, args)
            report.append((example.name, shot or "default", outcome))

    failures = [r for r in report if not r[2].startswith("captured")]
    print("", file=sys.stderr)
    for name, shot, outcome in report:
        line = f"  {name}  [{shot}]  {outcome}"
        print(line if outcome.startswith("captured") else console.dim(line), file=sys.stderr)
    print(console.dim(f"{len(report) - len(failures)}/{len(report)} captured"), file=sys.stderr)

    _refresh(ctx, preset, args.refresh_captures if args.refresh_captures is not None else args.update_captures)

    if failures:
        sys.exit(1)


def _registered_shots(ctx: Context, preset, example, artifact: Path, args: argparse.Namespace) -> list[str]:
    """The capture names an example registers, asked of the example itself.

    Registration happens while a frame is authored, so this runs one headless frame to find out.
    A run that fails to answer contributes no shots rather than failing the sweep: its default capture still runs, and
    that is where the real error will surface.
    """
    result = dev.run_step(
        [str(artifact), example.name, "--examples"],
        step_type="example", name=f"{example.target}-list",
        build_dir=preset.build_dir, cwd=_working_directory(ctx, example),
        env={**os.environ, "SC_CAPTURE": "1", "SC_CAPTURE_LIST": "1"},
        timeout=_SWEEP_TIMEOUT_SECONDS, mirror=False, verbose=args.verbose,
    )
    if result.returncode != 0:
        return []

    # The listing goes to stdout, which run_step captured to a file rather than handing back.
    # Only lines carrying the prefix count: the binary is a test runner too, and its own summary line goes to the same stream.
    # Taking every non-empty line made "All 1 tests passed" a capture name.
    printed = _read_log(result.stdout_log)
    return [line.strip().removeprefix(_LIST_PREFIX)
            for line in printed.splitlines() if line.strip().startswith(_LIST_PREFIX)]


def _capture_one(ctx: Context, preset, example, artifact: Path, shot: str, args: argparse.Namespace) -> str:
    """One capture, into its own folder under the build directory.

    Returns the line the report shows for it.
    """
    directory = capture_directory(preset, example.name, shot)
    directory.mkdir(parents=True, exist_ok=True)
    image = directory / "capture.jpg"

    env = {**os.environ, "SC_CAPTURE": "1", "SC_CAPTURE_SIZE": args.capture_size, "SC_CAPTURE_OUT": str(image)}
    if shot:
        env["SC_CAPTURE_NAME"] = shot
    if args.capture_accumulate is not None:
        env["SC_CAPTURE_ACCUMULATE"] = str(args.capture_accumulate)
    if args.capture_timeout is not None:
        env["SC_CAPTURE_TIMEOUT"] = str(args.capture_timeout)

    result = dev.run_step(
        [str(artifact), example.name, "--examples"],
        step_type="example", name=f"{example.target}-capture",
        build_dir=preset.build_dir, cwd=_working_directory(ctx, example), env=env,
        timeout=_SWEEP_TIMEOUT_SECONDS, mirror=False, verbose=args.verbose,
    )

    printed = _read_log(result.stdout_log)
    (directory / "stdout.txt").write_text(printed, encoding="utf-8")
    (directory / "stderr.txt").write_text(_read_log(result.stderr_log), encoding="utf-8")

    # An example that wrote no image is a text example, not a failure: SC_CAPTURE means nothing to one, so it simply ran.
    # Its transcript is the artifact, which is what makes the envelope — one flag, one folder, one report — genuinely
    # shared rather than an image feature with text bolted on.
    ok = result.returncode == 0
    if ok and image.is_file():
        main = image.name
    elif ok:
        main = "transcript.txt"
        (directory / main).write_text(printed, encoding="utf-8")
    else:
        main = None

    # The manifest names which artifact is the main one, so the refresh step needs to know nothing about example kinds.
    # `ok` is what keeps a failed capture out of the source tree: refresh copies nothing that did not succeed.
    (directory / "manifest.json").write_text(
        json.dumps({"example": example.name, "shot": shot, "main": main,
                    "source": example.file, "ok": ok}, indent=2),
        encoding="utf-8",
    )
    if main is not None:
        return f"captured -> {ctx.rel(directory / main)}"
    return f"FAILED (exit {result.returncode})"


def _read_log(path: Path) -> str:
    """A captured step's stream, or empty when it produced none."""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _refresh(ctx: Context, preset, match: str) -> None:
    """Copy each successful capture's main artifact next to the example that produced it.

    Separate from capturing on purpose.
    Capture writes only into the build directory, so it dirties nothing and anyone can run it at any time.
    This is the step that touches tracked files, and it is the one to run deliberately.
    """
    root = preset.build_dir / "captures"
    if not root.is_dir():
        print(console.dim("nothing captured yet"), file=sys.stderr)
        return

    copied = 0
    for manifest_path in sorted(root.rglob("manifest.json")):
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if not manifest.get("ok") or not manifest.get("main"):
            continue
        if match and match.lower() not in str(manifest.get("example", "")).lower():
            continue

        source = Path(manifest["source"])
        if not source.parent.is_dir():
            continue

        # Images only, for now.
        # A text example's transcript IS captured — it is in the build directory beside this manifest — but committing
        # one is the half that wants a CI diff to be worth anything, and that does not exist yet.
        # Copying them in now would add a tracked file per text example that nothing ever checks.
        suffix = Path(manifest["main"]).suffix.lower()
        if suffix not in (".jpg", ".jpeg", ".png"):
            continue

        shot = manifest.get("shot") or ""
        stem = source.stem + (f"-{shot}" if shot else "")
        destination = source.parent / f"{stem}{suffix}"
        shutil.copyfile(manifest_path.parent / manifest["main"], destination)
        print(console.dim(f"  refreshed {ctx.rel(destination)}"), file=sys.stderr)
        copied += 1

    print(console.dim(f"{copied} image(s) refreshed"), file=sys.stderr)


def _capture_environment(ctx: Context, preset, example, args) -> tuple[Path | None, dict]:
    """The environment a capture run needs, and the image it will write.

    The output path is composed here rather than inside the example, because dev.py is the only party that knows both
    the resolved example name and where this preset's build directory is.
    A capture never writes into the source tree: it lands under the build directory, which already carries the gitignore, the log archiving and the CI upload.
    Copying one next to its example is the separate refresh step.
    """
    env = {"SC_CAPTURE": "1", "SC_CAPTURE_SIZE": args.capture_size}
    if args.capture_accumulate is not None:
        env["SC_CAPTURE_ACCUMULATE"] = str(args.capture_accumulate)
    if args.capture_timeout is not None:
        env["SC_CAPTURE_TIMEOUT"] = str(args.capture_timeout)

    if args.capture_list:
        env["SC_CAPTURE_LIST"] = "1"
        return None, env

    shot = args.capture or ""
    if shot:
        env["SC_CAPTURE_NAME"] = shot

    if args.capture_out:
        out = Path(args.capture_out).resolve()
    else:
        out = capture_directory(preset, example.name, shot) / "capture.jpg"

    out.parent.mkdir(parents=True, exist_ok=True)
    env["SC_CAPTURE_OUT"] = str(out)
    return out, env


def _working_directory(ctx: Context, example) -> Path:
    """The directory an example runs in: the one its source sits in.

    An example's relative paths — the document it keeps, the assets it loads — should resolve next to the example
    rather than next to whatever the user happened to invoke dev.py from.
    That also lets the example's own directory carry the .gitignore for what a run leaves behind.

    Falls back to the repo root when the recorded source path is not on this machine, which is what a binary built
    elsewhere reports.
    """
    directory = Path(example.file).parent
    return directory if directory.is_dir() else ctx.root


def _print_listing(ctx: Context, examples: list) -> None:
    """Print every example, grouped by the binary carrying it."""
    if not examples:
        ctx.die("no examples found. Is SC_BUILD_EXAMPLES on, and has anything declared an EXAMPLE?")

    by_target: dict[str, list] = {}
    for e in examples:
        by_target.setdefault(e.target, []).append(e)

    for target in sorted(by_target):
        print(console.dim(target))
        for e in by_target[target]:
            print(f"  {e.name}")
            print(console.dim(f"      {ctx.rel(Path(e.file))}:{e.line}"))

    print(console.dim(f"\n{len(examples)} example(s). Run one with: dev.py example <name>"))
