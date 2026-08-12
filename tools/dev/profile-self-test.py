#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Invariant test for the profile machinery's pure half: lane allocation and the Chrome-tracing export.

Lanes are the one part of profiling that is *reconstructed* rather than measured, so they are the part that can be quietly wrong.
A lane that double-books two overlapping jobs makes a trace read as if work were serial when it was parallel — plausible, and invisible without a check like this.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/profile-self-test.py [-v]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.dev.lib.core import profile  # noqa: E402

verbose = False


class Failure(Exception):
    """An invariant did not hold."""


def check(condition: bool, what: str) -> None:
    if not condition:
        raise Failure(what)
    if verbose:
        print(f"  ok: {what}")


def job(name: str, type: str, start: float, end: float) -> profile.Job:
    return profile.Job(name=name, type=type, start=start, end=end)


def no_lane_double_books(jobs: list[profile.Job], epsilon: float) -> None:
    """Two jobs sharing a lane must not overlap by more than epsilon — the whole point of a lane."""
    by_lane: dict[tuple[str, int], list[profile.Job]] = {}
    for j in jobs:
        by_lane.setdefault((j.group, j.lane), []).append(j)
    for (group, lane), members in by_lane.items():
        members.sort(key=lambda j: j.start)
        for previous, current in zip(members, members[1:]):
            check(
                current.start + epsilon >= previous.end,
                f"lane {group}/{lane}: {current.name!r} starts before {previous.name!r} ends",
            )


def test_non_overlap() -> None:
    """Jobs that genuinely overlap get their own lanes; jobs that do not share one."""
    jobs = [
        job("a", "compile", 0.0, 1.0),
        job("b", "compile", 0.5, 1.5),
        job("c", "compile", 0.6, 1.6),
        job("d", "compile", 2.0, 3.0),
    ]
    lanes = profile.allocate_lanes(jobs, mode="global", epsilon=0.0)
    check(lanes == 3, f"three overlapping jobs need three lanes, got {lanes}")
    no_lane_double_books(jobs, epsilon=0.0)
    check(jobs[3].lane == 0, "a job starting after every lane freed reuses the first one")


def test_serial_chain_is_one_lane() -> None:
    """A strictly sequential run must collapse to a single lane, however long the chain."""
    jobs = [job(f"s{i}", "test", float(i), float(i) + 1.0) for i in range(50)]
    lanes = profile.allocate_lanes(jobs, mode="global", epsilon=0.0)
    check(lanes == 1, f"a serial chain is one lane, got {lanes}")


def test_epsilon_coalesces() -> None:
    """Two steps that end and start within epsilon share a lane rather than opening a second."""
    jobs = [job("first", "test", 0.0, 1.0), job("second", "test", 1.0 - 5e-5, 2.0)]

    lanes = profile.allocate_lanes(jobs, mode="global", epsilon=0.0)
    check(lanes == 2, "with no slack, a hair of overlap opens a second lane")

    jobs = [job("first", "test", 0.0, 1.0), job("second", "test", 1.0 - 5e-5, 2.0)]
    lanes = profile.allocate_lanes(jobs, mode="global", epsilon=profile.DEFAULT_LANE_EPSILON_S)
    check(lanes == 1, "within the default slack, the two share a lane")


def test_container_precedes_contained() -> None:
    """A job spanning others must land in a lower lane, so the trace reads as a flame chart.

    The parent is deliberately listed last, to prove the ordering comes from the sort and not from input order.
    """
    children = [job(f"tu{i}", "compile", 0.1 + i * 0.01, 0.9) for i in range(8)]
    jobs = [*children, job("build all", "build", 0.0, 1.0)]
    profile.allocate_lanes(jobs, mode="global", epsilon=0.0)

    parent = next(j for j in jobs if j.type == "build")
    check(parent.lane == 0, f"the containing job takes lane 0, got {parent.lane}")
    check(all(c.lane > 0 for c in children), "everything it contains sits above it")
    no_lane_double_books(jobs, epsilon=0.0)


def test_per_type_pools_are_independent() -> None:
    """Under per-type, a compile and a test that overlap each take lane 0 of their own pool."""
    jobs = [job("tu", "compile", 0.0, 1.0), job("suite", "test", 0.0, 1.0)]

    lanes = profile.allocate_lanes(jobs, mode="per-type", epsilon=0.0)
    check(lanes == 2, f"two pools of one lane each, got {lanes}")
    check({j.group for j in jobs} == {"compile", "test"}, "each job groups under its own type")
    check(all(j.lane == 0 for j in jobs), "neither pool is crowded by the other")

    jobs = [job("tu", "compile", 0.0, 1.0), job("suite", "test", 0.0, 1.0)]
    profile.allocate_lanes(jobs, mode="global", epsilon=0.0)
    check({j.group for j in jobs} == {"all"}, "under global every job shares one pool")
    check({j.lane for j in jobs} == {0, 1}, "and overlapping jobs are pushed apart")


def test_chrome_round_trip() -> None:
    """The exported trace keeps every job, its lane, and its duration."""
    jobs = [
        job("cfg", "configure", 100.0, 101.0),
        job("tu", "compile", 101.0, 101.5),
        job("other", "compile", 101.0, 101.25),
    ]
    profile.allocate_lanes(jobs, mode="per-type", epsilon=0.0)
    doc = profile.to_chrome_trace(jobs, argv=["dev.py", "build"])

    slices = [e for e in doc["traceEvents"] if e["ph"] == "X"]
    check(len(slices) == 3, f"one slice per job, got {len(slices)}")
    check(doc["displayTimeUnit"] == "ms", "the viewer is told the unit")

    first = next(e for e in slices if e["name"] == "cfg")
    check(first["ts"] == 0, f"the earliest job anchors the timeline at 0, got {first['ts']}")
    check(first["dur"] == 1_000_000, f"durations are microseconds, got {first['dur']}")
    check(first["cat"] == "configure", "the job type carries through as the category")

    tu = next(e for e in slices if e["name"] == "tu")
    check(tu["ts"] == 1_000_000, f"a later job is offset from the anchor, got {tu['ts']}")
    check(tu["pid"] != first["pid"], "separate pools render as separate processes")

    named = {e["args"]["name"] for e in doc["traceEvents"] if e.get("name") == "process_name"}
    check(named == {"configure", "compile"}, f"each pool is labelled, got {named}")


def test_union_span_counts_overlap_once() -> None:
    """The span column is the whole point of the summary, so its overlap merging has to be exact."""
    check(profile.union_span([]) == 0.0, "nothing spans no time")

    serial = [job("a", "t", 0.0, 1.0), job("b", "t", 1.0, 2.0)]
    check(profile.union_span(serial) == 2.0, "back-to-back jobs span their sum")

    overlapping = [job("a", "t", 0.0, 2.0), job("b", "t", 1.0, 3.0)]
    check(profile.union_span(overlapping) == 3.0, "overlap is counted once, not twice")

    nested = [job("outer", "t", 0.0, 10.0), job("inner", "t", 2.0, 3.0)]
    check(profile.union_span(nested) == 10.0, "a contained job adds nothing to the span")

    gapped = [job("a", "t", 0.0, 1.0), job("b", "t", 5.0, 6.0)]
    check(profile.union_span(gapped) == 2.0, "idle time between jobs is not spanned")

    # Deliberately out of order, since the merge walks a sorted copy rather than trusting the input.
    unsorted = [job("late", "t", 5.0, 6.0), job("early", "t", 0.0, 1.0)]
    check(profile.union_span(unsorted) == 2.0, "input order does not change the span")


def test_summary_separates_work_from_wall_clock() -> None:
    """Fanned-out work must show a large sum against a small span; the `all` row carries the run."""
    jobs = [job(f"tu{i}", "compile", 0.0, 1.0) for i in range(8)]
    jobs.append(job("build", "build", 0.0, 1.2))
    stats = profile.summarize(jobs)

    compile_row = next(s for s in stats if s.type == "compile")
    check(compile_row.count == 8, "every job of a type is counted")
    check(compile_row.total_s == 8.0, f"sum adds each job's duration, got {compile_row.total_s}")
    check(compile_row.span_s == 1.0, f"span counts the concurrent second once, got {compile_row.span_s}")
    check(abs(compile_row.parallelism - 8.0) < 1e-9, "and their ratio is the parallelism")

    total = stats[-1]
    check(total.type == "all", "the run-wide row comes last, where the printer expects it")
    check(total.count == 9, "it counts every job regardless of type")
    check(total.span_s == 1.2, f"and spans the whole run, got {total.span_s}")
    check(stats[0].total_s >= stats[1].total_s, "types are ordered heaviest first")

    check(profile.summarize([]) == [], "an empty profile summarizes to nothing at all")


def test_empty_trace() -> None:
    """Nothing recorded is a valid profile, not a crash."""
    doc = profile.to_chrome_trace([])
    check(doc["traceEvents"] == [], "an empty profile exports an empty trace")
    check(profile.allocate_lanes([], mode="global") == 0, "and needs no lanes")


def test_zero_length_job_stays_visible() -> None:
    """A job that took no measurable time still has to be clickable in the viewer."""
    jobs = [job("instant", "git", 5.0, 5.0)]
    profile.allocate_lanes(jobs, mode="global")
    doc = profile.to_chrome_trace(jobs)
    check(doc["traceEvents"][-1]["dur"] >= 1, "a zero-length slice is widened to 1 us")


TESTS = [
    test_non_overlap,
    test_serial_chain_is_one_lane,
    test_epsilon_coalesces,
    test_container_precedes_contained,
    test_per_type_pools_are_independent,
    test_union_span_counts_overlap_once,
    test_summary_separates_work_from_wall_clock,
    test_chrome_round_trip,
    test_empty_trace,
    test_zero_length_job_stays_visible,
]


def main() -> None:
    global verbose
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--verbose", "-v", action="store_true", help="Print each invariant as it passes")
    verbose = parser.parse_args().verbose

    failures = 0
    for t in TESTS:
        name = t.__name__.removeprefix("test_")
        try:
            t()
        except Failure as e:
            failures += 1
            print(f"FAIL {name}: {e}")
        else:
            print(f"ok   {name}")

    if failures:
        print(f"\n{failures} of {len(TESTS)} failed")
        sys.exit(1)
    print(f"\nall {len(TESTS)} passed")


if __name__ == "__main__":
    main()
