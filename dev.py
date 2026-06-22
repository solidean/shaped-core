#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""
Build & test CLI for shaped-core.

Usage:
    uv run dev.py configure                 Configure the CMake project
    uv run dev.py build [--target T]        Build (optionally specific targets)
    uv run dev.py test [--target T] [NAME]  Run tests (optionally a binary / test name)
    uv run dev.py clean [--all]             Remove build artifacts
    uv run dev.py doctor                    Sanity-check the toolchain
    uv run dev.py list-presets              List available build presets
    uv run dev.py list-targets              List discovered targets

Presets and targets accept comma-lists, repeated flags, and shell-style
wildcards, and operate on as many as you select:
    --preset debug-clang,release-clang
    --preset "x64-linux-*" --preset debug-clang
    --target "*-test"

The default preset is chosen by platform but can be overridden with --preset.
'build' and 'test' auto-configure when cmake inputs or the source listing change
(fingerprinted); pass --no-configure to skip.

dev.py is quiet by default: child stdout/stderr is captured to
build/<preset>/run-logs/run-log-<NN>-<step>.{stdout,stderr}.txt rather than
mirrored to the terminal — pass --mirror-output to stream it live. Each command
also writes a machine-readable sidecar (configure.json / build.json / test.json)
next to the build directory.
"""

from __future__ import annotations

import argparse
import fnmatch
import platform
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from tools import dev  # noqa: E402

# ---------------------------------------------------------------------------
# Project policy
# ---------------------------------------------------------------------------

# Default build preset per platform. Override with --preset.
DEFAULT_BUILD_PRESETS: dict[str, str] = {
    "Windows": "relwithdebinfo-clang",
    "Linux": "relwithdebinfo-linux-clang",
    "Darwin": "macos-arm-llvm-relwithdebinfo",
}


def _is_test_target(target: dev.Target) -> bool:
    """Project convention: test executables are named '*-test'."""
    return target.kind == "EXECUTABLE" and target.name.endswith("-test")


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def _rel(p: Path) -> str:
    """Path relative to the repo root (posix style) for compact hints."""
    try:
        return p.relative_to(ROOT).as_posix()
    except ValueError:
        return str(p)


def _fmt_dur(seconds: float) -> str:
    return f"{seconds * 1000:.0f} ms" if seconds < 1 else f"{seconds:.1f} s"


def _build_diag_hint(presets: list[dev.Preset]) -> str:
    if len(presets) == 1:
        return f'diagnose with: build_diag base_path="{_rel(presets[0].build_dir)}"'
    return "diagnose with: build_diag"


def _test_diag_hint(presets: list[dev.Preset]) -> str:
    if len(presets) == 1:
        sel = f"{_rel(presets[0].build_dir)}/**/*.results.xml"
    else:
        sel = "build/**/*.results.xml"
    return f'diagnose with: test_diag path="{sel}" errors_only=true'


def default_preset_name() -> str:
    name = DEFAULT_BUILD_PRESETS.get(platform.system())
    if name is None:
        die(f"No default preset for {platform.system()!r}. Use --preset.")
    return name


def resolve_presets(specs: list[str] | None) -> list[dev.Preset]:
    """Resolve --preset specs, falling back to the platform default."""
    try:
        presets = dev.resolve_presets(ROOT, specs or [])
        if not presets:
            presets = dev.resolve_presets(ROOT, [default_preset_name()])
        return presets
    except dev.PresetError as e:
        die(str(e))


def _discover(preset: dev.Preset) -> list[dev.Target]:
    """Discover targets for a preset, auto-configuring if needed."""
    try:
        return dev.discover_targets(preset.build_dir, preset.build_type)
    except dev.NotConfiguredError:
        result = dev.ensure_configured(preset, root=ROOT)
        if result is not None and not result.ok:
            die(f"Configure failed for {preset.name!r}")
        return dev.discover_targets(preset.build_dir, preset.build_type)


def resolve_target_names(preset: dev.Preset, specs: list[str] | None) -> list[str] | None:
    """Expand --target specs into concrete target names against a preset.

    Returns None when no specs were given (meaning 'build everything').
    """
    patterns: list[str] = []
    for spec in specs or []:
        patterns.extend(s.strip() for s in spec.split(",") if s.strip())
    if not patterns:
        return None

    available = _discover(preset)
    names = [t.name for t in available]
    selected: list[str] = []
    seen: set[str] = set()
    for pat in patterns:
        matches = [pat] if pat in names else [n for n in names if fnmatch.fnmatch(n, pat)]
        if not matches:
            die(f"No target matches {pat!r}. Available: {', '.join(sorted(names))}")
        for n in matches:
            if n not in seen:
                seen.add(n)
                selected.append(n)
    return selected


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_configure(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    results = dev.configure(
        presets, root=ROOT, force=True, mirror=args.mirror_output, verbose=args.verbose
    )
    sys.exit(0 if all(r.ok for r in results) else 1)


def cmd_build(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    # Targets are resolved against the first preset (target sets match across presets).
    target_names = resolve_target_names(presets[0], args.target) if not args.no_configure else None
    if target_names is None and args.target:
        # --no-configure: can't discover, pass the literal names through to cmake.
        target_names = [s.strip() for spec in args.target for s in spec.split(",") if s.strip()]

    results = dev.build(
        presets,
        target_names,
        root=ROOT,
        auto_configure=not args.no_configure,
        mirror=args.mirror_output,
        verbose=args.verbose,
    )
    build_steps = [r for r in results if r.label.startswith("build")]
    total_s = sum(r.duration_s for r in build_steps)
    if not all(r.ok for r in results):
        print(f"\nbuild failed - {_build_diag_hint(presets)}", file=sys.stderr)
        sys.exit(1)
    print(f"\nBuilt {len(build_steps)} target(s) in {_fmt_dur(total_s)}.", file=sys.stderr)
    sys.exit(0)


def cmd_test(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    primary = presets[0]

    # Optionally build first (incremental — fast when nothing changed).
    if not args.no_build:
        target_names = resolve_target_names(primary, args.target) if not args.no_configure else None
        results = dev.build(
            presets, target_names, root=ROOT,
            auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
        )
        if not all(r.ok for r in results):
            print(f"\nbuild failed - {_build_diag_hint(presets)}", file=sys.stderr)
            sys.exit(1)

    # Determine which test binaries to run and the optional test-name filter.
    all_targets = _discover(primary)
    test_targets = [t for t in all_targets if _is_test_target(t)]
    test_name: str | None = args.test_name

    if args.target:
        wanted = set(resolve_target_names(primary, args.target) or [])
        binary_names = [t.name for t in dev.executables(all_targets) if t.name in wanted]
        if not binary_names:
            die(f"No test binary matches --target {args.target}")
    elif test_name:
        # If test_name names a binary, run that whole binary; otherwise treat it
        # as a test-name filter applied across every test binary (binaries with
        # no match are skipped by the runner, not failed).
        named = next((t for t in dev.executables(all_targets) if t.name == test_name), None)
        if named is not None:
            binary_names = [named.name]
            test_name = None
        else:
            binary_names = [t.name for t in test_targets]
            if not binary_names:
                die("No test binaries found (expected '*-test' executables)")
    else:
        binary_names = [t.name for t in test_targets]
        if not binary_names:
            die("No test binaries found (expected '*-test' executables)")

    records = dev.test(
        presets,
        binary_names,
        root=ROOT,
        test_name=test_name,
        timeout=args.timeout if args.timeout else None,
        write_xml=not args.no_xml_reports,
        mirror=args.mirror_output,
        verbose=args.verbose,
    )

    if args.merged_xml_report:
        parts = [Path(r["artifact"]).parent / f"{Path(r['artifact']).name}.results.xml" for r in records]
        dev.merge_junit(parts, Path(args.merged_xml_report))

    total_s = sum(r["duration_s"] for r in records)
    failed = sum(1 for r in records if r["returncode"] != 0)
    if failed:
        print(f"\n{failed} of {len(records)} test run(s) failed in {_fmt_dur(total_s)}", file=sys.stderr)
        print(f"tests failed - {_test_diag_hint(presets)}", file=sys.stderr)
        sys.exit(1)
    print(f"\nAll {len(records)} test run(s) passed in {_fmt_dur(total_s)}.", file=sys.stderr)
    sys.exit(0)


def cmd_clean(args: argparse.Namespace) -> None:
    if args.all:
        print("clean --all: removing all build/ artifacts", file=sys.stderr)
        presets = dev.load_presets(ROOT)
    else:
        presets = resolve_presets(args.preset)
        print(f"clean: preset(s) {', '.join(p.name for p in presets)}", file=sys.stderr)

    removed_any = False
    for preset in presets:
        removed_any |= dev.remove_build_dir(preset.build_dir, dry_run=args.dry_run)
    if not removed_any:
        print("  nothing to remove (already clean)", file=sys.stderr)


def cmd_doctor(args: argparse.Namespace) -> None:
    checks = dev.doctor(ROOT, default_preset=DEFAULT_BUILD_PRESETS.get(platform.system()))
    all_ok = True
    for label, ok, detail in checks:
        mark = "OK  " if ok else "FAIL"
        all_ok &= ok
        print(f"  [{mark}] {label}: {detail}")
    sys.exit(0 if all_ok else 1)


def cmd_list_presets(args: argparse.Namespace) -> None:
    for p in dev.load_presets(ROOT):
        print(f"{p.name}  ({p.build_type or 'no build type'} -> {p.configure_preset})")


def cmd_list_targets(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    for t in _discover(presets[0]):
        artifact = f"  -> {t.artifact}" if t.artifact else ""
        print(f"{t.name}  [{t.kind}]{artifact}")


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _add_preset_arg(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--preset", action="append",
        help="Build preset(s): comma-list, repeatable, and shell-style wildcards "
             "(default: auto-detected by platform)",
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="shaped-core build & test CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    parser.add_argument("--mirror-output", action="store_true",
                        help="Stream child stdout/stderr live instead of only capturing it")

    sub = parser.add_subparsers(dest="command", required=True)

    cfg_p = sub.add_parser("configure", help="Configure the CMake project")
    _add_preset_arg(cfg_p)

    build_p = sub.add_parser("build", help="Build the project")
    _add_preset_arg(build_p)
    build_p.add_argument("--target", "-t", action="append",
                         help="Target(s) to build: comma-list, repeatable, wildcards")
    build_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")

    test_p = sub.add_parser("test", help="Run tests")
    _add_preset_arg(test_p)
    test_p.add_argument("--target", "-t", action="append",
                        help="Test binary target(s): comma-list, repeatable, wildcards")
    test_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    test_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    test_p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                        help="Per-binary timeout in seconds (default: 60; 0 disables). The binary is "
                             "killed and reported as failed if it exceeds it.")
    xml_group = test_p.add_mutually_exclusive_group()
    xml_group.add_argument("--merged-xml-report", metavar="FILE",
                           help="Also merge the per-binary JUnit XML into a single report at FILE")
    xml_group.add_argument("--no-xml-reports", action="store_true",
                           help="Do not write any JUnit XML result files (per-binary XML is on by default)")
    test_p.add_argument("test_name", nargs="?",
                        help="Specific test name or binary to run (auto-discovers the binary)")

    clean_p = sub.add_parser("clean", help="Remove build artifacts")
    _add_preset_arg(clean_p)
    clean_p.add_argument("--all", action="store_true", help="Remove every preset's build directory")
    clean_p.add_argument("--dry-run", action="store_true", help="Print what would be removed")

    sub.add_parser("doctor", help="Sanity-check the toolchain")

    lp = sub.add_parser("list-presets", help="List available build presets")
    _add_preset_arg(lp)
    lt = sub.add_parser("list-targets", help="List discovered targets")
    _add_preset_arg(lt)

    args = parser.parse_args()

    match args.command:
        case "configure":
            cmd_configure(args)
        case "build":
            cmd_build(args)
        case "test":
            cmd_test(args)
        case "clean":
            cmd_clean(args)
        case "doctor":
            cmd_doctor(args)
        case "list-presets":
            cmd_list_presets(args)
        case "list-targets":
            cmd_list_targets(args)


if __name__ == "__main__":
    main()
