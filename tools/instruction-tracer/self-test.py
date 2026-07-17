#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""End-to-end test for instruction-tracer: trace the fixture and check what came back.

The nexus suite (instruction-tracer-test) covers the pure parts — parsing, decoding, formatting.
None of it touches the part that can actually break in the field: launching a process under the
debug API, landing an int3 on a symbol, single-stepping it, and reading the PDB back. That needs a
real debuggee, so it lives here rather than in the suite.

Kept separate from `dev.py test` on purpose: this is Windows-x64-only and needs PDBs, so it is not
something the cross-platform suite can sweep.

The assertions are built to be optimizer-independent. They never pin a particular codegen shape —
only things the language and the fixture's noinline/volatile guarantee (the call happens, the frame
exists, the function returns), plus one relation *between* runs that no optimizer can break: three
traces of the same function must retire the same addresses.

    uv run tools/instruction-tracer/self-test.py [--preset <name>] [-v]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# The tracer needs a PDB for symbols and line numbers, so a release preset would degrade to raw
# addresses and fail invariants that name a symbol.
DEFAULT_PRESET = "relwithdebinfo-clang"

TRACER_TARGET = "instruction-tracer"
FIXTURE_TARGET = "instruction-tracer-fixture"

# The fixture calls each of these 1000x, so --skip 100 always has a hit to land on.
SYMBOL = "itrace_fixture_add"
# Matches both itrace_fixture_add and itrace_fixture_mul, so ambiguity is deterministic.
AMBIGUOUS_SYMBOL = "itrace_fixture_"

INSTRUCTION_RE = re.compile(r"^ {2}([0-9a-f]{8}`[0-9a-f]{8})  (.+?)(?:  ;.*)?$")
STACK_FRAME_RE = re.compile(r"^ {2}(\S+)")

verbose = False


class Failure(Exception):
    """An invariant did not hold."""


class Trace:
    """One `=== trace n/m ===` block of the tracer's output.

    `block` is the text *after* the "=== trace " marker, so it opens with "n/m: symbol ===".
    """

    def __init__(self, block: str) -> None:
        self.text = block
        self.entry = _field(block, "entry:")
        self.reason = _field(block, "trace ended:")
        self.hit = _field(block, "hit:")
        self.index = block.split(":", 1)[0].strip()

        self.addresses: list[str] = []
        self.instructions: list[str] = []
        for line in block.splitlines():
            if m := INSTRUCTION_RE.match(line):
                self.addresses.append(m.group(1))
                self.instructions.append(m.group(2).strip())

        self.stack: list[str] = []
        if "stack:" in block:
            after = block.split("stack:", 1)[1]
            for line in after.splitlines()[1:]:
                if not line.strip():
                    break
                if m := STACK_FRAME_RE.match(line):
                    self.stack.append(m.group(1))


def _field(block: str, prefix: str) -> str:
    for line in block.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return ""


def parse_traces(stdout: str) -> list[Trace]:
    parts = stdout.split("=== trace ")
    return [Trace(p) for p in parts[1:]]


def run(*args: str) -> subprocess.CompletedProcess[str]:
    if verbose:
        print(f"  $ {' '.join(args)}", flush=True)
    return subprocess.run(args, capture_output=True, text=True, cwd=REPO_ROOT, timeout=120)


def target_paths(preset: str) -> dict[str, Path]:
    """Ask dev.py where it put each binary, rather than guessing the build layout."""
    result = run("uv", "run", "dev.py", "list-targets", "--preset", preset)
    if result.returncode != 0:
        sys.exit(f"dev.py list-targets failed:\n{result.stdout}\n{result.stderr}")

    paths: dict[str, Path] = {}
    for line in result.stdout.splitlines():
        if "->" not in line:
            continue
        name, path = line.split("->", 1)
        paths[name.split()[0].strip()] = Path(path.strip())
    return paths


def build(preset: str) -> dict[str, Path]:
    print(f"building {TRACER_TARGET} + {FIXTURE_TARGET} ({preset}) ...", flush=True)
    result = run("uv", "run", "dev.py", "build", "--preset", preset,
                 "-t", TRACER_TARGET, "-t", FIXTURE_TARGET)
    if result.returncode != 0:
        sys.exit(f"build failed:\n{result.stdout}\n{result.stderr}")

    paths = target_paths(preset)
    for target in (TRACER_TARGET, FIXTURE_TARGET):
        if target not in paths or not paths[target].is_file():
            sys.exit(f"{target} was not built (looked for {paths.get(target)})")
    return paths


class Tracer:
    def __init__(self, tracer: Path, fixture: Path) -> None:
        self._tracer = tracer
        self._fixture = fixture

    def __call__(self, *args: str) -> tuple[int, str, str]:
        result = run(str(self._tracer), "--exe", str(self._fixture), *args)
        return result.returncode, result.stdout, result.stderr

    def traces(self, *args: str) -> list[Trace]:
        """Trace, requiring success. Returns the parsed trace blocks."""
        code, out, err = self(*args)
        if code != 0:
            raise Failure(f"tracer exited {code} for {' '.join(args)}\n{out}\n{err}")
        return parse_traces(out)


# --- the invariants ----------------------------------------------------------------------------
# Each takes the tracer and raises Failure. Keep them independent of any particular codegen.

def check_traces_one_invocation(t: Tracer) -> None:
    """--skip 100 lands on hit 101 of 1000, and --traces 1 records exactly one."""
    traces = t.traces("--symbol", SYMBOL, "--skip", "100", "--traces", "1")
    if len(traces) != 1:
        raise Failure(f"expected exactly 1 trace, got {len(traces)}")
    if traces[0].hit != "101":
        raise Failure(f"expected hit 101 after --skip 100, got {traces[0].hit!r}")


def check_entry_is_the_function_start(t: Tracer) -> None:
    """The breakpoint must land on the symbol itself, not somewhere inside it.

    A `+0x..` suffix would mean the entry address was off — the whole bp/skip/rearm dance is wrong.
    """
    trace = t.traces("--symbol", SYMBOL, "--skip", "100")[0]
    if not trace.entry.endswith(f"!{SYMBOL}"):
        raise Failure(f"entry is not the function start: {trace.entry!r}")


def check_returns(t: Tracer) -> None:
    """A noinline function must return, so --until-return must end on a ret."""
    trace = t.traces("--symbol", SYMBOL, "--skip", "100", "--until-return")[0]

    if "returned" not in trace.reason:
        raise Failure(f"expected the frame to return, got reason {trace.reason!r}")
    if not trace.instructions[-1].startswith("ret"):
        raise Failure(f"expected the last instruction to be a ret, got {trace.instructions[-1]!r}")


def check_minimum_work(t: Tracer) -> None:
    """noinline + two volatile locals cannot compile to fewer than a few instructions."""
    trace = t.traces("--symbol", SYMBOL, "--skip", "100")[0]
    if len(trace.instructions) < 3:
        raise Failure(f"expected >= 3 instructions, got {len(trace.instructions)}: {trace.instructions}")


def check_traces_are_reproducible(t: Tracer) -> None:
    """The strongest invariant here: three traces of the same branch-free function must retire the
    same addresses. It asserts a relation *between* runs, so no codegen choice can invalidate it —
    only the tracer being wrong (a missed step, a stale breakpoint, a bad rearm) can."""
    traces = t.traces("--symbol", SYMBOL, "--skip", "100", "--traces", "3")
    if len(traces) != 3:
        raise Failure(f"expected 3 traces, got {len(traces)}")

    first = traces[0].addresses
    for i, other in enumerate(traces[1:], start=2):
        if other.addresses != first:
            raise Failure(f"trace {i} diverged from trace 1:\n  {first}\n  {other.addresses}")

    if [x.index for x in traces] != ["1/3", "2/3", "3/3"]:
        raise Failure(f"traces are misnumbered: {[x.index for x in traces]}")


def check_stack(t: Tracer) -> None:
    """A noinline callee always has a real frame, and the fixture always calls it from drive()."""
    trace = t.traces("--symbol", SYMBOL, "--skip", "100", "--stack")[0]

    if len(trace.stack) < 2:
        raise Failure(f"expected >= 2 stack frames, got {trace.stack}")
    if trace.stack[0] != SYMBOL:
        raise Failure(f"expected {SYMBOL} at the top of the stack, got {trace.stack[0]!r}")
    if "drive" not in trace.stack:
        raise Failure(f"expected drive() on the stack, got {trace.stack}")


def check_ambiguity_is_reported(t: Tracer) -> None:
    """Two fixture functions share a prefix, so this spec can only ever be ambiguous."""
    code, out, err = t("--symbol", AMBIGUOUS_SYMBOL)
    combined = out + err

    if code == 0:
        raise Failure(f"an ambiguous symbol must not succeed:\n{combined}")
    if "ambiguous" not in combined:
        raise Failure(f"expected an ambiguity report, got:\n{combined}")
    for candidate in ("itrace_fixture_add", "itrace_fixture_mul"):
        if candidate not in combined:
            raise Failure(f"ambiguity report is missing {candidate}:\n{combined}")


def check_address_form_agrees(t: Tracer) -> None:
    """--address must reach the same place --symbol did.

    Safe within one run: Windows randomizes an image's base once per boot, not per launch.
    """
    by_symbol = t.traces("--symbol", SYMBOL, "--skip", "100")[0]
    address = "0x" + by_symbol.addresses[0].replace("`", "")

    by_address = t.traces("--address", address, "--skip", "100")[0]

    if by_address.addresses[0] != by_symbol.addresses[0]:
        raise Failure(f"--address {address} started at {by_address.addresses[0]}, "
                      f"--symbol started at {by_symbol.addresses[0]}")
    if by_address.entry != by_symbol.entry:
        raise Failure(f"--address resolved to {by_address.entry!r}, --symbol to {by_symbol.entry!r}")


def check_stats_replaces_the_trace(t: Tracer) -> None:
    """--stats prints a table instead of the trace, and charges the work to the traced symbol.

    Optimizer-independent: the fixture's noinline function must own at least one instruction, and its
    self count can never exceed the total.
    """
    code, out, err = t("--symbol", SYMBOL, "--skip", "100", "--stats")
    if code != 0:
        raise Failure(f"tracer exited {code} with --stats\n{out}\n{err}")

    if "=== trace " in out:
        raise Failure(f"--stats must replace the trace, not accompany it:\n{out}")
    for column in ("self", "atomics", "calls d/i", "mem r/w", "symbol"):
        if column not in out:
            raise Failure(f"--stats table is missing the {column!r} column:\n{out}")
    if "total (1 trace)" not in out:
        raise Failure(f"--stats table is missing its totals row:\n{out}")

    rows = [line for line in out.splitlines() if line.rstrip().endswith(SYMBOL)]
    if len(rows) != 1:
        raise Failure(f"expected exactly one {SYMBOL} row, got {len(rows)}:\n{out}")

    self_count = int(rows[0].split()[0])
    total = int(next(line for line in out.splitlines() if "total (" in line).split()[0])
    if self_count < 1:
        raise Failure(f"the traced function retired no instructions of its own:\n{out}")
    if self_count > total:
        raise Failure(f"self ({self_count}) exceeds the total ({total}):\n{out}")


def check_stats_counts_match_the_trace(t: Tracer) -> None:
    """The table's total must equal what the trace itself reported retiring.

    A relation between two renderings of one run: it holds whatever the fixture compiles to, and only
    a bucketing bug can break it.
    """
    trace = t.traces("--symbol", SYMBOL, "--skip", "100")[0]

    code, out, err = t("--symbol", SYMBOL, "--skip", "100", "--stats")
    if code != 0:
        raise Failure(f"tracer exited {code} with --stats\n{out}\n{err}")

    total = int(next(line for line in out.splitlines() if "total (" in line).split()[0])
    if total != len(trace.instructions):
        raise Failure(f"--stats totalled {total} instructions, the trace printed {len(trace.instructions)}")


def check_missing_symbol_fails(t: Tracer) -> None:
    """A symbol that cannot exist must fail loudly rather than trace nothing quietly."""
    code, out, err = t("--symbol", "itrace_definitely_not_a_real_symbol")
    if code == 0:
        raise Failure(f"a missing symbol must not succeed:\n{out}\n{err}")


CHECKS = [
    ("records exactly one invocation, at the skipped-to hit", check_traces_one_invocation),
    ("breaks exactly on the function entry", check_entry_is_the_function_start),
    ("--until-return ends on a ret", check_returns),
    ("records the work the fixture cannot optimize away", check_minimum_work),
    ("repeated traces retire identical addresses", check_traces_are_reproducible),
    ("captures the entry stack", check_stack),
    ("reports an ambiguous symbol with candidates", check_ambiguity_is_reported),
    ("--address agrees with --symbol", check_address_form_agrees),
    ("--stats tables the run instead of tracing it", check_stats_replaces_the_trace),
    ("--stats totals agree with the trace", check_stats_counts_match_the_trace),
    ("a missing symbol fails loudly", check_missing_symbol_fails),
]


def main() -> int:
    global verbose

    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--preset", default=DEFAULT_PRESET, help=f"build preset (default: {DEFAULT_PRESET})")
    ap.add_argument("-v", "--verbose", action="store_true", help="echo every command")
    args = ap.parse_args()
    verbose = args.verbose

    if sys.platform != "win32":
        print("self-test.py is Windows-only (the tracer uses the Win32 debug API)")
        return 0

    paths = build(args.preset)
    tracer = Tracer(paths[TRACER_TARGET], paths[FIXTURE_TARGET])

    print(f"\nrunning {len(CHECKS)} checks against {paths[FIXTURE_TARGET].name} ...\n", flush=True)

    failures = 0
    for description, check in CHECKS:
        try:
            check(tracer)
            print(f"  ok    {description}", flush=True)
        except Failure as e:
            failures += 1
            print(f"  FAIL  {description}\n        {e}", flush=True)

    print()
    if failures:
        print(f"{failures} of {len(CHECKS)} checks failed")
        return 1

    print(f"all {len(CHECKS)} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
