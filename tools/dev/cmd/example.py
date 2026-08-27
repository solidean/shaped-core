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
from tools.dev.lib.pipeline.captures import IMAGE_MECHANISMS, SIDECAR_NAME, SidecarError, captures_for
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
                        "With NAME, take the capture of that name. Both must be declared in the example's "
                        ".capture.json sidecar; an example without one is not capturable.")
    p.add_argument("--capture-out", metavar="PATH",
                   help="Where --capture writes. Defaults to build/<preset>/captures/<name>/<shot>/capture.jpg; "
                        "the extension picks the format.")
    p.add_argument("--capture-size", metavar="WxH",
                   help="Override the resolution the sidecar declares")
    p.add_argument("--capture-accumulate", metavar="N", type=int,
                   help="Override the accumulated-frame target the sidecar declares")
    p.add_argument("--capture-timeout", metavar="SECS", type=float,
                   help="Override the timeout the sidecar declares")
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
    if args.capture is not None:
        capture = _one_capture(ctx, example, args.capture, args)
        capture_path = _capture_output(preset, capture, args)
        env = {**(env or os.environ), **_capture_environment(capture, capture_path, args)}

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
        # A capture the binary refused — an unregistered name, a target it could not read back — is reported by
        # logging and closing the loop, so a clean exit alone does not mean an image exists.
        if capture.mechanism in IMAGE_MECHANISMS and not capture_path.is_file():
            ctx.die(f"capture produced no image at {ctx.rel(capture_path)} — see the run's stderr above")
        print(console.dim(f"capture -> {ctx.rel(capture_path)}"), file=sys.stderr)

    sys.exit(result.returncode)


# A sweep bounds each run: the interactive default of no timeout is right for a window somebody is watching and
# wrong for a corpus refresh, where one example that never settles would hang the whole thing.
_SWEEP_TIMEOUT_SECONDS = 300.0



def _sweep(ctx: Context, preset, targets, examples: list, args: argparse.Namespace) -> None:
    """Capture every declared capture of every example the matcher selects, then refresh the ones that worked.

    What it iterates is the SIDECARS, not the examples.
    An example with no `.capture.json` entry is never launched, so the sweep genuinely opens no window — which is what
    separates it from the `--all` that docs/guides/examples.md refuses, rather than a claim nobody checks.

    One process per capture, so each starts from an identical cold state.
    """
    selected = select_examples(examples, args.update_captures)
    if not selected:
        ctx.die(f"no example matches {args.update_captures!r}")

    by_target = {t.name: t for t in targets}
    report: list[tuple[str, str, str]] = []
    skipped = 0

    for example in selected:
        try:
            declared = captures_for(example, ctx.root)
        except SidecarError as e:
            ctx.die(str(e))

        if declared is None:
            skipped += 1
            continue

        target = by_target.get(example.target)
        artifact = target.artifact if target else None
        if artifact is None:
            report.append((example.name, "-", "skipped: no built artifact"))
            continue

        for capture in declared:
            outcome = _capture_one(ctx, preset, example, artifact, capture, args)
            report.append((example.name, capture.slug, outcome))

    if not report:
        ctx.die("nothing to capture: no selected example declares a .capture.json entry")

    failures = [r for r in report if not r[2].startswith("captured")]
    print("", file=sys.stderr)
    for name, shot, outcome in report:
        line = f"  {name}  [{shot}]  {outcome}"
        print(line if outcome.startswith("captured") else console.dim(line), file=sys.stderr)
    print(console.dim(f"{len(report) - len(failures)}/{len(report)} captured"
                      + (f", {skipped} example(s) declare no capture" if skipped else "")), file=sys.stderr)

    _refresh(ctx, preset, args.refresh_captures if args.refresh_captures is not None else args.update_captures)

    if failures:
        sys.exit(1)


def _one_capture(ctx: Context, example, name: str, args: argparse.Namespace):
    """The one capture `name` selects out of an example's sidecar.

    A name the sidecar does not declare is an error here rather than a run that quietly takes the default view.
    The binary makes the same refusal for a name nothing registered, so both halves of a rename fail loudly.
    """
    try:
        declared = captures_for(example, ctx.root)
    except SidecarError as e:
        ctx.die(str(e))

    if declared is None:
        ctx.die(f"{example.name} declares no captures: add an entry to {Path(example.file).parent / SIDECAR_NAME}")

    for capture in declared:
        if capture.name == name:
            return capture

    offered = ", ".join(c.slug for c in declared)
    ctx.die(f"{example.name} declares no capture named {name!r}; it declares: {offered}")


def _capture_output(preset, capture, args: argparse.Namespace) -> Path:
    """Where one capture's image goes, honouring an explicit --capture-out."""
    if args.capture_out:
        out = Path(args.capture_out).resolve()
    else:
        out = capture_directory(preset, capture.example, capture.name) / f"capture.{capture.fmt}"

    out.parent.mkdir(parents=True, exist_ok=True)
    return out


def _capture_environment(capture, out: Path, args: argparse.Namespace) -> dict:
    """The environment one capture run needs.

    The sidecar carries the defaults and the flags override them, so iterating on framing never means editing a file.
    """
    env = {"SC_CAPTURE": "1", "SC_CAPTURE_OUT": str(out), "SC_CAPTURE_SIZE": args.capture_size or capture.size}
    if capture.name:
        env["SC_CAPTURE_NAME"] = capture.name

    accumulate = args.capture_accumulate if args.capture_accumulate is not None else capture.accumulate
    if accumulate is not None:
        env["SC_CAPTURE_ACCUMULATE"] = str(accumulate)

    timeout = args.capture_timeout if args.capture_timeout is not None else capture.timeout
    if timeout is not None:
        env["SC_CAPTURE_TIMEOUT"] = str(timeout)

    return env


def _capture_one(ctx: Context, preset, example, artifact: Path, capture, args: argparse.Namespace) -> str:
    """One capture, into its own folder under the build directory.

    Returns the line the report shows for it.
    """
    image = _capture_output(preset, capture, args)
    directory = image.parent

    result = dev.run_step(
        [str(artifact), example.name, "--examples"],
        step_type="example", name=f"{example.target}-capture",
        build_dir=preset.build_dir, cwd=_working_directory(ctx, example),
        env={**os.environ, **_capture_environment(capture, image, args)},
        timeout=_SWEEP_TIMEOUT_SECONDS, mirror=False, verbose=args.verbose,
    )

    printed = _read_log(result.stdout_log)
    (directory / "stdout.txt").write_text(printed, encoding="utf-8")
    (directory / "stderr.txt").write_text(_read_log(result.stderr_log), encoding="utf-8")

    # What counts as the artifact is the MECHANISM's answer, never a guess from what appeared.
    # An `sv` capture that produced no image failed, even on a clean exit — the binary reports a capture it could not
    # take by logging and closing the loop, so the exit code alone would call that a success.
    # Guessing instead would file a failed image capture as a successful transcript, which is a plausible-looking lie.
    expects_image = capture.mechanism in IMAGE_MECHANISMS
    ok = result.returncode == 0 and (image.is_file() if expects_image else True)
    if not ok:
        main = None
    elif expects_image:
        main = image.name
    else:
        main = "transcript.txt"
        (directory / main).write_text(printed, encoding="utf-8")

    # The manifest names which artifact is the main one, so the refresh step needs to know nothing about example kinds.
    # `ok` is what keeps a failed capture out of the source tree: refresh copies nothing that did not succeed.
    (directory / "manifest.json").write_text(
        json.dumps({"example": example.name, "shot": capture.name, "main": main,
                    "source": example.file, "ok": ok}, indent=2),
        encoding="utf-8",
    )
    if main is not None:
        return f"captured -> {ctx.rel(directory / main)}"
    if result.returncode == 0:
        return "FAILED (the run wrote no image — see stderr.txt beside it)"
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
