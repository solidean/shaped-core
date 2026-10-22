"""Generic pre-commit check runner.

A `Check` is a named gate the CLI can run, and the registry of which gates exist is cmd/check.py's.
This module only knows how to *run* a selected set, with a colored verdict at the end.
Select by what the change touched, build what the gates need once, run the read-only gates concurrently, then the fixers in order, then the slow `requires_green` tail.
"""

from __future__ import annotations

import threading
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from ..core import console, profile, ui
from ..core.report import fmt_dur
from .changes import ChangeScope, touched_paths
from .watch import Watch, tracked_files


class CheckRegistryError(Exception):
    """A gate declared wrongly, carrying the message the CLI should surface."""


@dataclass
class Check:
    name: str
    description: str
    supports_fix: bool
    run: Callable[..., bool]
    # The slow tail: run only after every static check is green (see run_checks).
    requires_green: bool = False

    #: The repo-relative globs this gate is about; it runs when the change touches one of them.
    #: Exactly one of `watches` and `always` is set: "always run" is the safe default and the slow one, so a gate states it rather than falls into it.
    watches: tuple[str, ...] | None = None
    always: bool = False

    #: Gates sharing a tag never run at the same time: "gpu" for anything that opens a device.
    exclusive: str | None = None

    #: Targets this gate needs built, on the default preset.
    #: The runner builds every selected gate's targets in one invocation before any gate runs, so a gate never builds on its own.
    builds: tuple[str, ...] = ()

    _watch: Watch | None = field(default=None, init=False, repr=False, compare=False)

    def __post_init__(self) -> None:
        if self.always == (self.watches is not None):
            raise CheckRegistryError(
                f"check {self.name!r} must declare exactly one of watches=(...) and always=True")
        if self.watches is not None:
            if not self.watches:
                raise CheckRegistryError(f"check {self.name!r} watches nothing, so it would never run")
            self._watch = Watch.of(self.watches)

    def touched_by(self, paths: list[str]) -> list[str]:
        """Which of `paths` this gate is about; everything for an `always` gate."""
        return list(paths) if self._watch is None else self._watch.touched_by(paths)


def validate_checks(checks: list[Check], *, root: Path) -> None:
    """Refuse a glob that matches no tracked file: the file it named has moved, and the gate would silently stop running."""
    tracked = tracked_files(root)
    if not tracked:
        return
    dead = [(c.name, p) for c in checks if c._watch is not None for p in c._watch.dead_patterns(tracked)]
    if dead:
        lines = "\n".join(f"  {name}: {pattern!r}" for name, pattern in dead)
        raise CheckRegistryError(f"check: these watch globs match no tracked file, so the rule is stale:\n{lines}")


def list_checks(checks: list[Check]) -> None:
    """Print one line per registered check, tagging --fix / needs-green support and what it watches."""
    for c in checks:
        tags = []
        if c.supports_fix:
            tags.append("--fix")
        if c.requires_green:
            tags.append("needs-green")
        if c.exclusive:
            tags.append(f"exclusive:{c.exclusive}")
        if c.builds:
            tags.append(f"builds {', '.join(c.builds)}")
        suffix = f"  [{', '.join(tags)}]" if tags else ""
        print(f"{c.name}{suffix}  {c.description}")
        print("    " + ("always" if c.always else "watches " + ", ".join(c.watches or ())))


def select_checks(checks: list[Check], *, root: Path, scope: ChangeScope | None) -> tuple[list[Check], list[str] | None]:
    """The gates a change selects, and the touched paths that selected them; None for a whole-tree scope.

    A whole-tree scope (`--all`) and a named gate bypass selection; this is for the registry walk alone.
    """
    if scope is None:
        return list(checks), None
    paths = touched_paths(root, scope)
    return [c for c in checks if c.always or c.touched_by(paths)], paths


def run_checks(
    selected: list[Check],
    *,
    fix: bool,
    scope: ChangeScope | None,
    mirror: bool,
    verbose: bool,
    no_test: bool,
    build: Callable[[list[str]], bool] | None = None,
    skipped: list[Check] | None = None,
) -> bool:
    """Run the selected checks and print a verdict; return True if all passed.

    Every target the selected gates declare is built first, in one invocation, through `build`; a failed build fails those gates and no other.
    The read-only gates then run concurrently, bounded by their `exclusive` tags.
    The gates that write under `--fix` run after them in registry order, since a fixer's rewrite is what the later ones must see.
    Without `--fix` nothing writes, so every static gate is read-only.
    A `requires_green` check runs last, only if no static gate failed and `no_test` is False: there is no point building and testing a tree that already fails a cheap lint.
    `scope` is handed to every check identically; a check that is always repo-wide ignores it.
    `skipped` is reported, so a gate selection is visible rather than silent.
    """
    failed: list[str] = []
    timings: list[tuple[str, float]] = []
    lock = threading.Lock()
    exclusive: dict[str, threading.Lock] = {}

    for c in skipped or []:
        ui.write_line(console.dim(f"--- skipped {c.name} (nothing it watches changed) ---"))

    static = [c for c in selected if not c.requires_green]
    writers = [c for c in static if fix and c.supports_fix]
    readers = [c for c in static if c not in writers]

    # One build for every static gate that needs one, ahead of them all: two ninja processes in one build directory
    # fight over the cores and over ninja's own bookkeeping, so a gate never builds for itself.
    unbuilt: set[str] = set()
    targets = sorted({t for c in static for t in c.builds})
    if targets and build is not None:
        ui.write_line(console.dim(f"\n--- building for {', '.join(c.name for c in static if c.builds)} ---"))
        started = time.monotonic()
        with profile.span("gate builds", type="check-gate"):
            ok = build(targets)
        timings.append(("build", time.monotonic() - started))
        if not ok:
            unbuilt = {c.name for c in static if c.builds}
            failed.extend(sorted(unbuilt))

    # The gate list is known upfront, so the region can say which gate of how many is running.
    with ui.phase("check", total=len(selected)) as gates:

        def run_one(c: Check) -> None:
            if c.name in unbuilt:
                ui.write_line(console.red(f"\n--- skipped {c.name} (its build failed) ---"))
                gates.advance(c.name)
                return
            ui.write_line(console.dim(f"\n--- running {c.name} ---"))
            gates.advance(c.name)
            started = time.monotonic()
            with lock:
                guard = exclusive.setdefault(c.exclusive, threading.Lock()) if c.exclusive else None
            try:
                if guard is not None:
                    guard.acquire()
                with profile.span(c.name, type="check-gate"):
                    ok = c.run(fix=fix, scope=scope, mirror=mirror, verbose=verbose)
            except Exception as e:  # noqa: BLE001 - a gate that raises is a failed gate, and the rest still run
                ui.write_line(console.red(f"{c.name}: {type(e).__name__}: {e}"))
                ok = False
            finally:
                if guard is not None:
                    guard.release()
            with lock:
                timings.append((c.name, time.monotonic() - started))
                if not ok:
                    failed.append(c.name)

        # Mirrored output from several gates at once would interleave into noise, so those runs stay serial.
        workers = 1 if (mirror or verbose) else max(1, len(readers))
        with ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(run_one, readers))
        for c in writers:
            run_one(c)

        for c in selected:
            if not c.requires_green:
                continue
            if no_test:
                ui.write_line(console.yellow(f"\n--- skipped {c.name} (--no-test) ---"))
                gates.advance(c.name)
            elif failed:
                ui.write_line(console.yellow(f"\n--- skipped {c.name} (static checks failed) ---"))
                gates.advance(c.name)
            else:
                run_one(c)

    # One line for the static gates, so a linter or a formatter quietly becoming a time sink is visible rather than
    # buried in the total.
    # The test gate prints its own per-preset breakdown and is left out of this.
    # The gates ran concurrently, so the total is work rather than wall clock.
    static_t = [(name, secs) for name, secs in timings if name != "test"]
    if static_t:
        parts = ", ".join(f"{name} {fmt_dur(secs)}" for name, secs in static_t)
        ui.write_line(console.dim(f"\nstatic gates: {parts}  (work {fmt_dur(sum(s for _, s in static_t))})"))

    if failed:
        ui.write_line(console.red("\ncheck: FAIL"))
        for name in sorted(set(failed)):
            ui.write_line(console.red(f"  - {name}"))
        return False
    ui.write_line(console.green("\ncheck: OK"))
    return True
