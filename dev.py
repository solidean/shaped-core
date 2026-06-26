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
    uv run dev.py test-web [LIBRARY]        Open the browser test runner (Emscripten; all libs, or one)
    uv run dev.py format [--dirty-only]     Format libs/ sources with clang-format
    uv run dev.py check [NAME...] [--fix]   Run pre-commit checks (format, crossrefs, test)
    uv run dev.py coverage run [NAME]       Collect LLVM test coverage (run/merge/report)
    uv run dev.py clean [--all]             Remove build artifacts
    uv run dev.py diagnose clangd FILE      Show clangd diagnostics for a source file
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
build/<preset>/run-logs/run-log-<name>.{stdout,stderr}.txt rather than
mirrored to the terminal — pass --mirror-output to stream it live. Each command
also writes a machine-readable sidecar (configure.json / build.json / test.json)
next to the build directory.
"""

from __future__ import annotations

import argparse
import fnmatch
import platform
import shutil
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
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

# Debug sibling of each default preset. Run by the `test` check alongside the
# others: -O0 plus mimalloc's debug heap (MI_DEBUG) catch bugs the optimized
# presets miss.
DEFAULT_DEBUG_PRESETS: dict[str, str] = {
    "Windows": "debug-clang",
    "Linux": "debug-linux-clang",
    "Darwin": "macos-arm-llvm-debug",
}

# Release sibling of each default preset, run alongside it by the `test` check so
# precommit exercises both CC_ASSERT on (debug/relwithdebinfo) and off (release).
DEFAULT_RELEASE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

# Sanitizer (ASan+UBSan) preset per platform, run by the `test` check as an extra
# defensive pass. Windows is intentionally absent: clang-cl's ASan is broken with
# C++ exceptions (any throw/catch faults during EH dispatch — a toolchain bug, not
# ours), and nexus catches test exceptions, so it can never be green there. The
# sanitize-clang preset still exists for manual non-throwing runs; see
# docs/guides/building-and-testing.md.
DEFAULT_SANITIZE_PRESETS: dict[str, str] = {
    "Linux": "sanitize-linux-clang",
    "Darwin": "sanitize-macos-arm-llvm",
}

# Default coverage preset per platform (instrumented Debug build; SC_COVERAGE ON).
# `coverage` uses these instead of DEFAULT_BUILD_PRESETS when no --preset is given.
COVERAGE_BUILD_PRESETS: dict[str, str] = {
    "Windows": "coverage-clang",
    "Linux": "coverage-linux-clang",
    "Darwin": "coverage-macos-arm-llvm",
}

# Default preset for `test-web` (the browser runner is Emscripten-only, regardless of host platform).
DEFAULT_WEB_PRESET = "emscripten-relwithdebinfo"


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


def _print_build_failure(results: list[dev.StepResult], presets: list[dev.Preset]) -> None:
    """Report a failed build phase with the right diagnosis hint.

    A configure failure leaves no per-translation-unit sidecars, so the
    build_diag hint would point at an empty scan. Point at the captured configure
    log instead; only genuine compile/link failures get the build_diag hint.
    """
    cfg_fail = next((r for r in results if not r.ok and r.step_type == "configure"), None)
    if cfg_fail is not None:
        print(f"\nconfigure failed - see {_rel(cfg_fail.stderr_log)}", file=sys.stderr)
    else:
        print(f"\nbuild failed - {_build_diag_hint(presets)}", file=sys.stderr)


def _fail_build(results: list[dev.StepResult], presets: list[dev.Preset]) -> None:
    """Report a failed build phase and exit(1) (for the build/test commands)."""
    _print_build_failure(results, presets)
    sys.exit(1)


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


def _discover(preset: dev.Preset, emsdk_path: str | None = None) -> list[dev.Target]:
    """Discover targets for a preset, auto-configuring if needed."""
    try:
        return dev.discover_targets(preset.build_dir, preset.build_type)
    except dev.NotConfiguredError:
        result = dev.ensure_configured(preset, root=ROOT, emsdk_path=emsdk_path)
        if result is not None and not result.ok:
            die(f"Configure failed for {preset.name!r}")
        return dev.discover_targets(preset.build_dir, preset.build_type)


def resolve_target_names(
    preset: dev.Preset, specs: list[str] | None, emsdk_path: str | None = None
) -> list[str] | None:
    """Expand --target specs into concrete target names against a preset.

    Returns None when no specs were given (meaning 'build everything').
    """
    patterns: list[str] = []
    for spec in specs or []:
        patterns.extend(s.strip() for s in spec.split(",") if s.strip())
    if not patterns:
        return None

    available = _discover(preset, emsdk_path)
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
        presets, root=ROOT, force=True, mirror=args.mirror_output, verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )
    sys.exit(0 if all(r.ok for r in results) else 1)


def cmd_build(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    # Targets are resolved against the first preset (target sets match across presets).
    target_names = (
        resolve_target_names(presets[0], args.target, args.emsdk_path)
        if not args.no_configure else None
    )
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
        emsdk_path=args.emsdk_path,
    )
    build_steps = [r for r in results if r.step_type == "build"]
    total_s = sum(r.duration_s for r in build_steps)
    if not all(r.ok for r in results):
        _fail_build(results, presets)
    files = sum(dev.ninja_built_count(r.stdout_log) for r in build_steps)
    print(
        f"\nBuilt {files} file(s) across {len(build_steps)} target(s), "
        f"{len(presets)} preset(s) in {_fmt_dur(total_s)}.",
        file=sys.stderr,
    )
    sys.exit(0)


def cmd_test(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    primary = presets[0]

    # Optionally build first (incremental — fast when nothing changed).
    if not args.no_build:
        target_names = (
            resolve_target_names(primary, args.target, args.emsdk_path)
            if not args.no_configure else None
        )
        results = dev.build(
            presets, target_names, root=ROOT,
            auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
            emsdk_path=args.emsdk_path,
        )
        if not all(r.ok for r in results):
            _fail_build(results, presets)

    # Determine which test binaries to run and the optional test-name filter.
    all_targets = _discover(primary, args.emsdk_path)
    test_targets = [t for t in all_targets if _is_test_target(t)]
    test_name: str | None = args.test_name

    if args.target:
        wanted = set(resolve_target_names(primary, args.target, args.emsdk_path) or [])
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
        emsdk_path=args.emsdk_path,
    )

    if args.merged_xml_report:
        parts = [Path(r["artifact"]).parent / f"{Path(r['artifact']).name}.results.xml" for r in records]
        dev.merge_junit(parts, Path(args.merged_xml_report))

    sys.exit(0 if _summarize_tests(records, presets) else 1)


def _summarize_tests(records: list[dict], presets: list[dev.Preset]) -> bool:
    """Print the pass/fail summary for a set of test runs; return True if all
    passed. Shared by the `test` command and the `test` check."""
    total_s = sum(r["duration_s"] for r in records)
    failed = sum(1 for r in records if r["returncode"] != 0)
    tests = sum(r["junit"]["tests"] for r in records if r["junit"])
    checks = sum(r["junit"]["assertions"] for r in records if r["junit"])
    stats = f"{tests} tests, {checks} checks"
    if failed:
        print(
            f"\n{failed} of {len(records)} test run(s) failed ({stats}) in {_fmt_dur(total_s)}",
            file=sys.stderr,
        )
        print(f"tests failed - {_test_diag_hint(presets)}", file=sys.stderr)
        return False
    print(
        f"\nAll {len(records)} test run(s) passed: {stats} in {_fmt_dur(total_s)}.",
        file=sys.stderr,
    )
    return True


def cmd_test_web(args: argparse.Namespace) -> None:
    # The browser test runner is Emscripten-only. No library arg -> the aggregate page (all libraries);
    # a library arg -> just that library's page. Either way we build the module(s), then serve+open the
    # page with emrun (the page loads its wasm test module(s) and runs them one per animation frame).
    presets = resolve_presets(args.preset or [DEFAULT_WEB_PRESET])
    preset = presets[0]
    if not preset.is_emscripten:
        die(f"test-web needs an Emscripten preset (got {preset.name!r}); e.g. --preset {DEFAULT_WEB_PRESET}")

    if args.library:
        # Accept "clean-core", "clean-core-test", or "clean-core-test-web" — all name the same runner.
        lib = args.library.removesuffix("-web").removesuffix("-test")
        target: str | None = f"{lib}-test-web"
        page = f"{lib}-web.html"
    else:
        target = None  # build everything so all modules + the aggregate page are present
        page = "tests-web.html"

    if not args.no_build:
        results = dev.build(
            presets, [target] if target else None, root=ROOT,
            mirror=args.mirror_output, verbose=args.verbose, emsdk_path=args.emsdk_path,
        )
        if not all(r.ok for r in results):
            _fail_build(results, presets)

    page_path = preset.build_dir / page
    if not page_path.is_file():
        die(f"no page at {_rel(page_path)} - "
            + (f"library {args.library!r} has no web runner?" if args.library else "build may have failed"))

    env = dev.emsdk_env(args.emsdk_path)
    if env is None:
        die("emsdk not found - pass --emsdk-path or activate emsdk (see: uv run dev.py doctor)")
    emrun = shutil.which("emrun", path=env.get("PATH"))
    if emrun is None:
        die("emrun not found in the emsdk environment")

    # emrun serves the page's directory (the build root) so the page can reach its libs/ modules, and
    # opens the default browser. It runs in the foreground until you stop it (Ctrl-C). .bat needs cmd.
    launch = ["cmd", "/c", emrun] if emrun.lower().endswith((".bat", ".cmd")) else [emrun]
    print(f"serving {_rel(page_path)} via emrun (Ctrl-C to stop)...", file=sys.stderr)
    result = subprocess.run([*launch, str(page_path)], env=env, cwd=str(preset.build_dir))
    sys.exit(result.returncode)


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


def _run_format(
    *,
    check: bool,
    dirty_only: bool,
    allow_different_version: bool,
    mirror: bool,
    verbose: bool,
) -> bool:
    """Run clang-format over the selected libs/ sources; return True on success.

    In check mode reports non-conforming files and returns False if any differ;
    otherwise rewrites in place. "Nothing to format" counts as success. Exits the
    process on unrecoverable setup errors (clang-format missing, or a version
    mismatch without allow_different_version) — those can't be reported per-file.
    Shared by `cmd_format` and the `format` check.
    """
    clang_format = dev.find_clang_format()
    if clang_format is None:
        die("clang-format not found on PATH. Install LLVM/clang-format (>= 21) or add it to PATH.")

    # clang-format output is not stable across major versions, so enforce the
    # major declared by .clang-format. allow_different_version downgrades the
    # mismatch to a warning instead of failing.
    have = dev.clang_format_version(clang_format)
    need = dev.required_major(ROOT)
    if have is None:
        die(f"could not determine clang-format version from {clang_format!r}")
    if have[0] != need:
        have_str = ".".join(str(p) for p in have)
        msg = (f"clang-format major version {have[0]} != required {need} "
               f"(found {have_str}); formatting may differ from the pinned style")
        if allow_different_version:
            print(f"WARNING: {msg}", file=sys.stderr)
        else:
            die(f"{msg}. Install clang-format {need}.x, or pass --allow-different-version to proceed anyway.")

    files = dev.discover_files(ROOT, dirty_only=dirty_only)
    if not files:
        scope = "dirty libs/ sources" if dirty_only else "libs/ sources"
        print(f"No {scope} to format.", file=sys.stderr)
        return True

    result = dev.format_sources(
        files, root=ROOT, clang_format=clang_format,
        check=check, mirror=mirror, verbose=verbose,
    )

    if check:
        if result.ok:
            print(f"\n{len(files)} file(s) already formatted.", file=sys.stderr)
            return True
        offenders = dev.violating_files(result, ROOT)
        for f in offenders:
            print(_rel(f))
        sys.stdout.flush()
        print(
            f"\n{len(offenders)} of {len(files)} file(s) need formatting "
            f"- run: uv run dev.py format" + (" --dirty-only" if dirty_only else ""),
            file=sys.stderr,
        )
        return False

    if not result.ok:
        print(f"\nformat failed - see {_rel(result.stderr_log)}", file=sys.stderr)
        return False
    print(
        f"\nFormatted {len(files)} file(s) in {_fmt_dur(result.duration_s)}.",
        file=sys.stderr,
    )
    return True


def cmd_format(args: argparse.Namespace) -> None:
    ok = _run_format(
        check=args.check_only,
        dirty_only=args.dirty_only,
        allow_different_version=args.allow_different_version,
        mirror=args.mirror_output,
        verbose=args.verbose,
    )
    sys.exit(0 if ok else 1)


# ---------------------------------------------------------------------------
# Pre-commit checks
#
# Each registered Check is a named gate `dev.py check` can run. `dev.py check`
# with no name runs them all (the pre-commit aggregator); `dev.py check <name>`
# runs a subset. A check's `run` prints its own banner/summary and returns
# ok: bool. Checks that support `--fix` apply unambiguous fixes (clang-format);
# others ignore it and only report. A `requires_green` check (the test suite)
# is the slow tail: it runs only after every static check passed — there's no
# point building and testing a tree that already fails a cheap lint — and
# `--no-test` skips it outright (handy for a docs-only re-check). The registry
# is the growth point — new gates plug in here without touching the command
# surface.
# ---------------------------------------------------------------------------

def _check_format(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
    # Pre-commit default is dirty-only (just the next commit's files); --all
    # widens to the whole tree. --fix rewrites in place, else check-only.
    return _run_format(
        check=not fix,
        dirty_only=not all_scope,
        allow_different_version=False,
        mirror=mirror,
        verbose=verbose,
    )


def _check_crossrefs(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
    # Always full-repo: a moved file breaks links in other, untouched files, so a
    # dirty-only scan would miss exactly the breakage this guards against. Not
    # fixable, so fix/all_scope are ignored.
    result = dev.check_crossrefs(ROOT)
    files = result.md_files + result.src_files
    if not result.ok:
        for offender in result.offenders:
            print(offender)
        sys.stdout.flush()
        print(
            f"\n{len(result.offenders)} stale or broken cross-reference(s) across {files} file(s)",
            file=sys.stderr,
        )
        return False
    total = result.md_links + result.src_refs
    print(
        f"\nOK: {total} cross-references valid across {files} files "
        f"({result.md_links} md links, {result.src_refs} source refs)",
        file=sys.stderr,
    )
    return True


def _check_tests(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
    # Build + run the full suite across build variants: debug (-O0 + mimalloc's
    # MI_DEBUG heap) and relwithdebinfo exercise CC_ASSERT on, release exercises
    # it off, and — where supported — a sanitizer (ASan+UBSan) preset adds a
    # memory/UB pass. Together they catch allocator misuse, assert regressions,
    # and undefined behavior the optimized presets miss. Warm builds are the norm
    # at commit time — uncompiled code couldn't have been tested — so the real
    # cost is the test run. Not fixable; fix/all_scope ignored.
    system = platform.system()
    specs = [default_preset_name()]
    for sibling in (
        DEFAULT_DEBUG_PRESETS.get(system),
        DEFAULT_RELEASE_PRESETS.get(system),
        DEFAULT_SANITIZE_PRESETS.get(system),
    ):
        if sibling:
            specs.append(sibling)
    presets = resolve_presets(specs)

    results = dev.build(presets, None, root=ROOT, auto_configure=True, mirror=mirror, verbose=verbose)
    if not all(r.ok for r in results):
        _print_build_failure(results, presets)
        return False

    test_targets = [t for t in _discover(presets[0]) if _is_test_target(t)]
    if not test_targets:
        print("No test binaries found (expected '*-test' executables)", file=sys.stderr)
        return False

    records = dev.test(
        presets, [t.name for t in test_targets], root=ROOT,
        test_name=None, timeout=60.0, write_xml=True, mirror=mirror, verbose=verbose,
    )
    return _summarize_tests(records, presets)


@dataclass
class Check:
    name: str
    description: str
    supports_fix: bool
    run: Callable[..., bool]
    # The slow tail: run only after every static check is green (see cmd_check).
    requires_green: bool = False


CHECKS: list[Check] = [
    Check("format", "clang-format libs/ sources (dirty-only; --all for the whole tree)",
          True, _check_format),
    Check("crossrefs", "validate doc<->code cross-references repo-wide", False, _check_crossrefs),
    Check("test", "build + run the full suite on the debug, default, release (and where supported, sanitizer) presets",
          False, _check_tests, requires_green=True),
]


def cmd_check(args: argparse.Namespace) -> None:
    if args.list:
        for c in CHECKS:
            tags = []
            if c.supports_fix:
                tags.append("--fix")
            if c.requires_green:
                tags.append("needs-green")
            suffix = f"  [{', '.join(tags)}]" if tags else ""
            print(f"{c.name}{suffix}  {c.description}")
        sys.exit(0)

    by_name = {c.name: c for c in CHECKS}
    if args.names:
        selected: list[Check] = []
        seen: set[str] = set()
        for name in args.names:
            if name not in by_name:
                die(f"unknown check {name!r}. Available: {', '.join(by_name)}")
            if name not in seen:
                seen.add(name)
                selected.append(by_name[name])
    else:
        selected = list(CHECKS)

    def run_one(c: Check) -> None:
        print(f"\n--- running {c.name} ---", file=sys.stderr)
        if not c.run(fix=args.fix, all_scope=args.all, mirror=args.mirror_output, verbose=args.verbose):
            failed.append(c.name)

    # Static checks first; the slow `requires_green` tail (the test suite) runs
    # only if they all passed and --no-test wasn't given.
    failed: list[str] = []
    for c in selected:
        if not c.requires_green:
            run_one(c)
    for c in selected:
        if not c.requires_green:
            continue
        if args.no_test:
            print(f"\n--- skipped {c.name} (--no-test) ---", file=sys.stderr)
        elif failed:
            print(f"\n--- skipped {c.name} (static checks failed) ---", file=sys.stderr)
        else:
            run_one(c)

    if failed:
        print("\ncheck: FAIL", file=sys.stderr)
        for name in failed:
            print(f"  - {name}", file=sys.stderr)
        sys.exit(1)
    print("\ncheck: OK", file=sys.stderr)
    sys.exit(0)


def cmd_diagnose(args: argparse.Namespace) -> None:
    if args.diagnose_target == "clangd":
        cmd_diagnose_clangd(args)
    else:  # argparse 'required=True' should prevent this.
        die(f"unknown diagnose target {args.diagnose_target!r}")


def cmd_diagnose_clangd(args: argparse.Namespace) -> None:
    file = Path(args.file)
    if not file.is_absolute():
        file = Path.cwd() / file
    file = file.resolve()
    if not file.is_file():
        die(f"no such file: {args.file}")

    clangd_bin = dev.clangd.find_clangd()
    if clangd_bin is None:
        die("clangd not found on PATH. Install LLVM/clangd or add it to PATH.")

    # Default: reproduce the editor exactly — let clangd discover the database the
    # same way it does in the IDE (via .clangd's CompilationDatabase and its own
    # upward search). That way a misconfigured .clangd shows up here too, instead
    # of being masked. With --preset, force that preset's per-preset database.
    if args.preset:
        preset = resolve_presets(args.preset)[0]
        cc_dir = preset.build_dir
        label = f"{preset.name} ({_rel(cc_dir)})"
        if not (cc_dir / "compile_commands.json").is_file():
            die(f"no compile_commands.json at {_rel(cc_dir)} - run: uv run dev.py configure --preset {preset.name}")
    else:
        cc_dir = None
        label = "clangd auto-discovery (.clangd)"

    print(f"clangd: checking {_rel(file)} against {label}", file=sys.stderr)
    try:
        result = dev.clangd.check_file(clangd_bin, file, compile_commands_dir=cc_dir, timeout=120)
    except subprocess.TimeoutExpired:
        die("clangd --check timed out")

    if not result.found_database:
        print(
            f"WARNING: clangd found no compilation database for {_rel(file)} and used generic "
            f"fallback flags (no project includes), so diagnostics below are unreliable. This is "
            f"the same failure the editor hits. Check .clangd's CompilationDatabase points at the "
            f"build/ directory, and run: uv run dev.py configure",
            file=sys.stderr,
        )

    if args.verbose:
        print(result.log, file=sys.stderr)

    rel = _rel(file)
    for d in result.diagnostics:
        print(f"{rel}:{d.line}: {d.severity}: {d.message} [{d.code}]")
    sys.stdout.flush()  # keep diagnostics (stdout) ahead of the summary (stderr)

    errors = len(result.errors)
    warnings = len(result.warnings)
    if not result.diagnostics:
        print("\nNo diagnostics.", file=sys.stderr)
    else:
        print(f"\n{errors} error(s), {warnings} warning(s)", file=sys.stderr)
    sys.exit(1 if errors else 0)


def cmd_doctor(args: argparse.Namespace) -> None:
    checks = dev.doctor(
        ROOT, default_preset=DEFAULT_BUILD_PRESETS.get(platform.system()), emsdk_path=args.emsdk_path
    )
    all_ok = True
    for label, ok, detail in checks:
        # ok is True (pass), False (fail), or None for an advisory (neither).
        if ok is None:
            mark = "SKIP"
        elif ok:
            mark = "OK  "
        else:
            mark = "FAIL"
            all_ok = False
        print(f"  [{mark}] {label}: {detail}")
    sys.exit(0 if all_ok else 1)


def cmd_list_presets(args: argparse.Namespace) -> None:
    for p in dev.load_presets(ROOT):
        print(f"{p.name}  ({p.build_type or 'no build type'} -> {p.configure_preset})")


def cmd_list_targets(args: argparse.Namespace) -> None:
    presets = resolve_presets(args.preset)
    for t in _discover(presets[0], args.emsdk_path):
        artifact = f"  -> {t.artifact}" if t.artifact else ""
        print(f"{t.name}  [{t.kind}]{artifact}")


# ---------------------------------------------------------------------------
# Coverage
#
# `coverage` collects LLVM source-based coverage from the instrumented
# *-coverage presets. Three phases map to subcommands: `run` (build + run tests
# + merge + report), `merge` (combine several presets' merged data), and
# `report` (re-post-process existing data without re-running). The raw
# `llvm-cov export` JSON lands as a `.llvm-cov.json` sidecar in the build dir for
# future tooling (a `coverage_diag` analog of build_diag/test_diag).
# ---------------------------------------------------------------------------

def default_coverage_preset_name() -> str:
    name = COVERAGE_BUILD_PRESETS.get(platform.system())
    if name is None:
        die(f"No default coverage preset for {platform.system()!r}. Use --preset.")
    return name


def _resolve_coverage_presets(specs: list[str] | None) -> list[dev.Preset]:
    """Resolve --preset for coverage, defaulting to the platform coverage preset."""
    return resolve_presets(specs or [default_coverage_preset_name()])


def _select_test_binaries(
    preset: dev.Preset, target_specs: list[str] | None, name_arg: str | None
) -> tuple[list[str], str | None]:
    """Pick which test binaries to run and the optional test-name filter.

    Mirrors `cmd_test`: --target selects binaries; a positional that names a
    binary runs just that one; otherwise the positional is a name filter applied
    across every '*-test'.
    """
    all_targets = _discover(preset)
    test_targets = [t for t in all_targets if _is_test_target(t)]
    if target_specs:
        wanted = set(resolve_target_names(preset, target_specs) or [])
        names = [t.name for t in dev.executables(all_targets) if t.name in wanted]
        if not names:
            die(f"No test binary matches --target {target_specs}")
        return names, None
    if name_arg:
        named = next((t for t in dev.executables(all_targets) if t.name == name_arg), None)
        if named is not None:
            return [named.name], None
        names = [t.name for t in test_targets]
        if not names:
            die("No test binaries found (expected '*-test' executables)")
        return names, name_arg
    names = [t.name for t in test_targets]
    if not names:
        die("No test binaries found (expected '*-test' executables)")
    return names, None


def _summarize_coverage(results: list[dict]) -> bool:
    """Print the per-preset coverage tables; return True if every step succeeded."""
    ok = True
    for r in results:
        if not r["ok"]:
            ok = False
            failed = next((s for s in r["steps"] if not s.ok), None)
            where = f" - see {_rel(failed.stderr_log)}" if failed else ""
            print(f"\ncoverage [{r['preset']}] FAILED{where}", file=sys.stderr)
            continue

        t = r["totals"]
        def pct(metric: str) -> str:
            return f"{t.get(metric, {}).get('percent', 0.0):.1f}%"
        lines = t.get("lines", {})
        print(
            f"\nCoverage [{r['preset']}]: lines {pct('lines')} "
            f"({lines.get('covered', 0)}/{lines.get('count', 0)}), "
            f"functions {pct('functions')}, regions {pct('regions')}",
            file=sys.stderr,
        )
        for lib, m in r["libraries"].items():
            lm = m.get("lines", {})
            print(
                f"  {lib:<30} {lm.get('percent', 0.0):6.1f}%  "
                f"({lm.get('covered', 0)}/{lm.get('count', 0)} lines)",
                file=sys.stderr,
            )
        print(f"  JSON: {_rel(r['llvm_cov_json'])}", file=sys.stderr)
        if r["html_dir"]:
            print(f"  HTML: {_rel(Path(r['html_dir']) / 'index.html')}", file=sys.stderr)
    return ok


def cmd_coverage(args: argparse.Namespace) -> None:
    match args.coverage_cmd:
        case "run":
            cmd_coverage_run(args)
        case "merge":
            cmd_coverage_merge(args)
        case "report":
            cmd_coverage_report(args)
        case _:  # argparse 'required=True' should prevent this.
            die(f"unknown coverage subcommand {args.coverage_cmd!r}")


def cmd_coverage_run(args: argparse.Namespace) -> None:
    presets = _resolve_coverage_presets(args.preset)
    primary = presets[0]

    if not args.no_build:
        results = dev.build(
            presets, None, root=ROOT, auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
        )
        if not all(r.ok for r in results):
            _fail_build(results, presets)

    binary_names, test_name = _select_test_binaries(primary, args.target, args.pattern)
    try:
        cov_results = dev.coverage_run(
            presets, binary_names, root=ROOT, test_name=test_name, html=args.html,
            timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.CoverageToolError as e:
        die(str(e))
    sys.exit(0 if _summarize_coverage(cov_results) else 1)


def cmd_coverage_report(args: argparse.Namespace) -> None:
    presets = _resolve_coverage_presets(args.preset)
    binary_names = [t.name for t in _discover(presets[0]) if _is_test_target(t)]
    try:
        results = dev.coverage_report(
            presets, binary_names, root=ROOT, html=args.html,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except (dev.CoverageToolError, FileNotFoundError) as e:
        die(str(e))
    sys.exit(0 if _summarize_coverage(results) else 1)


def cmd_coverage_merge(args: argparse.Namespace) -> None:
    if not args.preset:
        die("coverage merge needs at least one --preset (which presets' data to combine)")
    presets = resolve_presets(args.preset)
    names_by_preset = {p.name: [t.name for t in _discover(p) if _is_test_target(t)] for p in presets}
    output_dir = Path(args.output) if args.output else (ROOT / "build" / "coverage-merged")
    try:
        result = dev.coverage_merge(
            presets, names_by_preset, root=ROOT, output_dir=output_dir, html=args.html,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except (dev.CoverageToolError, FileNotFoundError) as e:
        die(str(e))
    sys.exit(0 if _summarize_coverage([result]) else 1)


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _add_preset_arg(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--preset", action="append",
        help="Build preset(s): comma-list, repeatable, and shell-style wildcards "
             "(default: auto-detected by platform)",
    )


def _add_emsdk_arg(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--emsdk-path", metavar="DIR", default=None,
        help="Path to an emsdk install for the WASM (Emscripten) presets; dev.py applies its "
             "environment itself, so no permanent/--system activation is needed. Falls back to "
             "SC_EMSDK_PATH / EMSDK / emcc-on-PATH.",
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
    _add_emsdk_arg(cfg_p)

    build_p = sub.add_parser("build", help="Build the project")
    _add_preset_arg(build_p)
    _add_emsdk_arg(build_p)
    build_p.add_argument("--target", "-t", action="append",
                         help="Target(s) to build: comma-list, repeatable, wildcards")
    build_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")

    test_p = sub.add_parser("test", help="Run tests")
    _add_preset_arg(test_p)
    _add_emsdk_arg(test_p)
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

    web_p = sub.add_parser("test-web", help="Open the browser test runner (Emscripten); serves + opens via emrun")
    _add_preset_arg(web_p)
    _add_emsdk_arg(web_p)
    web_p.add_argument("library", nargs="?",
                       help="Library to show alone (e.g. clean-core); omit for the combined page of all libraries")
    web_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")

    fmt_p = sub.add_parser("format", help="Format C++ sources with clang-format")
    fmt_p.add_argument("--check-only", action="store_true",
                       help="Report non-conforming files and exit non-zero; do not rewrite")
    fmt_p.add_argument("--dirty-only", action="store_true",
                       help="Only format git-dirty/untracked libs/ sources (good pre-commit check)")
    fmt_p.add_argument("--allow-different-version", action="store_true",
                       help="Downgrade a clang-format version mismatch from error to warning")

    check_p = sub.add_parser("check", help="Run pre-commit checks (format, crossrefs, ...)")
    check_p.add_argument("names", nargs="*", help="Specific check(s) to run (default: all)")
    check_p.add_argument("--fix", action="store_true",
                         help="Let fixable checks apply unambiguous fixes (e.g. clang-format -i)")
    check_p.add_argument("--all", action="store_true",
                         help="Widen the format check from dirty-only to the whole tree")
    check_p.add_argument("--no-test", action="store_true",
                         help="Skip the test suite (build + run); just the static checks")
    check_p.add_argument("--list", action="store_true", help="List registered checks and exit")

    cov_p = sub.add_parser("coverage", help="Collect LLVM source-based test coverage")
    cov_sub = cov_p.add_subparsers(dest="coverage_cmd", required=True)

    cov_run_p = cov_sub.add_parser("run", help="Build + run instrumented tests, then merge & report")
    _add_preset_arg(cov_run_p)
    cov_run_p.add_argument("--target", "-t", action="append",
                           help="Test binary target(s): comma-list, repeatable, wildcards")
    cov_run_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    cov_run_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    cov_run_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")
    cov_run_p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                           help="Per-binary timeout in seconds (default: 60; 0 disables)")
    cov_run_p.add_argument("pattern", nargs="?",
                           help="Specific test name or binary to run (auto-discovers the binary)")

    cov_merge_p = cov_sub.add_parser("merge", help="Combine several presets' coverage into one report")
    _add_preset_arg(cov_merge_p)
    cov_merge_p.add_argument("--output", "-o", metavar="DIR",
                             help="Output directory (default: build/coverage-merged)")
    cov_merge_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")

    cov_report_p = cov_sub.add_parser("report", help="Re-post-process existing coverage (no test run)")
    _add_preset_arg(cov_report_p)
    cov_report_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")

    clean_p = sub.add_parser("clean", help="Remove build artifacts")
    _add_preset_arg(clean_p)
    clean_p.add_argument("--all", action="store_true", help="Remove every preset's build directory")
    clean_p.add_argument("--dry-run", action="store_true", help="Print what would be removed")

    diag_p = sub.add_parser("diagnose", help="Diagnose tooling (e.g. clangd) for a file")
    diag_sub = diag_p.add_subparsers(dest="diagnose_target", required=True)
    clangd_p = diag_sub.add_parser(
        "clangd", help="Show clangd diagnostics for a file (uses build/compile_commands.json)"
    )
    _add_preset_arg(clangd_p)
    clangd_p.add_argument(
        "file", help="Source file to check (its compile flags come from the compilation database)"
    )

    doctor_p = sub.add_parser("doctor", help="Sanity-check the toolchain")
    _add_emsdk_arg(doctor_p)

    lp = sub.add_parser("list-presets", help="List available build presets")
    _add_preset_arg(lp)
    lt = sub.add_parser("list-targets", help="List discovered targets")
    _add_preset_arg(lt)
    _add_emsdk_arg(lt)

    args = parser.parse_args()

    match args.command:
        case "configure":
            cmd_configure(args)
        case "build":
            cmd_build(args)
        case "test":
            cmd_test(args)
        case "test-web":
            cmd_test_web(args)
        case "format":
            cmd_format(args)
        case "check":
            cmd_check(args)
        case "coverage":
            cmd_coverage(args)
        case "clean":
            cmd_clean(args)
        case "diagnose":
            cmd_diagnose(args)
        case "doctor":
            cmd_doctor(args)
        case "list-presets":
            cmd_list_presets(args)
        case "list-targets":
            cmd_list_targets(args)


if __name__ == "__main__":
    main()
