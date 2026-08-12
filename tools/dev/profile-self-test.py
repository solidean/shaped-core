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


def job(name: str, type: str, start: float, end: float, origin: str = "external") -> profile.Job:
    return profile.Job(name=name, type=type, start=start, end=end, origin=origin)


def driver(name: str, type: str, start: float, end: float) -> profile.Job:
    return profile.Job(name=name, type=type, start=start, end=end, origin="driver")


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
    check({j.group for j in jobs} == {profile.EXTERNAL_GROUP}, "under global every job shares one pool")
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


def test_trace_separates_driver_from_fan_out() -> None:
    """The driver's nested timeline and the fanned-out work are two processes, driver first."""
    jobs = [
        driver("check", "invocation", 0.0, 10.0),
        driver("all", "build", 1.0, 9.0),
        job("a.cc", "compile", 2.0, 8.0),
        job("b.cc", "compile", 2.0, 8.0),
    ]
    profile.lay_out(jobs, mode="global")
    doc = profile.to_chrome_trace(jobs)

    processes = {e["pid"]: e["args"]["name"] for e in doc["traceEvents"] if e.get("name") == "process_name"}
    check(set(processes.values()) == {profile.DRIVER_GROUP, profile.EXTERNAL_GROUP},
          f"exactly two processes, got {set(processes.values())}")
    check(processes[min(processes)] == profile.DRIVER_GROUP,
          "the driver process is emitted first, so it lands on top")

    slices = {e["name"]: e for e in doc["traceEvents"] if e["ph"] == "X"}
    check(slices["check"]["pid"] == slices["all"]["pid"], "driver jobs share one process")
    check(slices["a.cc"]["pid"] == slices["b.cc"]["pid"], "and the fan-out shares the other")
    check(slices["check"]["pid"] != slices["a.cc"]["pid"], "the two never mix")

    check(slices["check"]["tid"] == 0 and slices["all"]["tid"] == 1,
          "driver rows are nesting depth, so the viewer draws the call stack")
    check({slices["a.cc"]["tid"], slices["b.cc"]["tid"]} == {0, 1},
          "two concurrent compiles take two lanes of their own process")

    rows = {e["args"]["name"] for e in doc["traceEvents"] if e.get("name") == "thread_name"}
    check(rows == {"depth 0", "depth 1", "lane 0", "lane 1"}, f"rows say which layout they are, got {rows}")

    check(slices["all"]["args"]["leaf"] is False, "a container is marked as such in the viewer's args")
    check(slices["a.cc"]["args"]["leaf"] is True, "and a leaf likewise")


def test_classify_finds_containers() -> None:
    """A driver span enclosing another job is an aggregate; overlap alone never is."""
    outer = driver("outer", "build", 0.0, 10.0)
    inner = job("inner", "compile", 1.0, 2.0)
    profile.classify([outer, inner])
    check(outer.aggregate and not outer.is_leaf, "a step containing another job is an aggregate")
    check(inner.is_leaf, "and what it contains stays a leaf")

    a, b = driver("a", "t", 0.0, 2.0), driver("b", "t", 1.0, 3.0)
    profile.classify([a, b])
    check(a.is_leaf and b.is_leaf, "partial overlap is not containment — neither encloses the other")

    lone = driver("lone", "t", 0.0, 1.0)
    profile.classify([lone])
    check(lone.is_leaf, "a single job encloses nothing")

    # Identical intervals are peers.
    # Calling either one the container would drop the other out of the leaf table.
    twin_a, twin_b = driver("twin-a", "t", 0.0, 1.0), driver("twin-b", "t", 0.0, 1.0)
    profile.classify([twin_a, twin_b])
    check(twin_a.is_leaf and twin_b.is_leaf, "jobs sharing an interval are both leaves")

    # The realistic version: a whole batch landing on one clock tick, inside a step that really does contain them.
    batch = [job(f"tu{i}", "compile", 1.0, 2.0) for i in range(32)]
    step = driver("build", "build", 0.0, 3.0)
    profile.classify([step, *batch])
    check(all(b.is_leaf for b in batch), "every job of an identical batch stays a leaf")
    check(step.aggregate, "and the step around them is still the container")

    # Touching, not nesting: a sibling starting exactly when the previous ended.
    first, second = driver("first", "t", 0.0, 1.0), driver("second", "t", 1.0, 2.0)
    profile.classify([first, second])
    check(first.is_leaf and second.is_leaf, "back-to-back siblings are both leaves")


def test_concurrent_fan_out_never_contains_itself() -> None:
    """A long compile enclosing a short one is two cores, not structure — external jobs are always leaves.

    Reading that overlap as containment would file most of a build's fan-out under `containers`, leaving the leaf table reporting a fraction of the real work.
    """
    slow = job("slow.cc", "compile", 0.0, 6.0)
    quick = job("quick.cc", "compile", 1.0, 1.4)
    step = driver("all", "build", 0.0, 7.0)
    profile.classify([step, slow, quick])

    check(slow.is_leaf, "the long compile is not a container, however much it spans")
    check(quick.is_leaf, "and neither is the one inside its window")
    check(step.aggregate, "only the driver step that spawned them is")

    summary = profile.summarize([step, slow, quick])
    compiles = next(s for s in summary.leaves if s.type == "compile")
    check(compiles.count == 2, f"both compiles reach the leaf table, got {compiles.count}")
    check(abs(compiles.total_s - 6.4) < 1e-9, f"with their full sum, got {compiles.total_s}")


def test_driver_depth_follows_the_call_stack() -> None:
    """Driver jobs nest exactly, so depth must reproduce the call stack that produced them."""
    invocation = driver("check", "invocation", 0.0, 100.0)
    gate = driver("test", "check-gate", 1.0, 90.0)
    build = driver("all", "build", 2.0, 50.0)
    sibling = driver("suite", "test", 60.0, 80.0)
    edge = job("tu.cc", "compile", 3.0, 4.0)

    jobs = [invocation, gate, build, sibling, edge]
    tracks = profile.lay_out(jobs, mode="global")

    check(invocation.depth == 0, f"the outermost span is depth 0, got {invocation.depth}")
    check(gate.depth == 1, f"the gate inside it is depth 1, got {gate.depth}")
    check(build.depth == 2, f"the build inside that is depth 2, got {build.depth}")
    check(sibling.depth == 2, f"a later sibling of the build is also depth 2, got {sibling.depth}")

    check(all(j.group == profile.DRIVER_GROUP for j in (invocation, gate, build, sibling)),
          "every driver job belongs to the driver group")
    check(all(j.lane == j.depth for j in (invocation, gate, build, sibling)),
          "and its track is its depth, not an allocated lane")

    check(edge.group == profile.EXTERNAL_GROUP, "the harvested edge belongs to the external group")
    check(edge.lane == 0, f"and is laned independently of the step that spawned it, got lane {edge.lane}")
    check(tracks == 4, f"three driver depths plus one external lane, got {tracks}")


def test_adjacent_siblings_stay_siblings() -> None:
    """Two spans recorded back to back are siblings, even when the first appears to end a hair late.

    Timestamps are taken at span entry and exit, and the rounding between them can leave the previous span ending marginally after the next begins.
    Unwinding on that alone would bury every following sibling one row deeper than it belongs.
    """
    parent = driver("check", "invocation", 0.0, 10.0)
    first = driver("fingerprint", "fingerprint", 1.0, 2.0)
    second = driver("discover", "discover", 2.0 - 1e-5, 3.0)
    third = driver("env", "env", 3.5, 4.0)
    profile.classify([parent, first, second, third])

    check(parent.depth == 0, "the enclosing span is still depth 0")
    check(first.depth == 1, f"the first child is depth 1, got {first.depth}")
    check(second.depth == 1, f"and a sibling overlapping it by a hair stays depth 1, got {second.depth}")
    check(third.depth == 1, f"as does a cleanly separated later sibling, got {third.depth}")

    # A genuine child must still nest, so the tolerance above cannot have flattened everything.
    real_child = driver("inner", "configure", 1.2, 1.4)
    profile.classify([parent, first, real_child])
    check(real_child.depth == 2, f"a span truly inside another is deeper, got {real_child.depth}")


def test_split_summary_does_not_double_count() -> None:
    """The whole point of the split: leaf rows are addable, containers are not mixed in."""
    jobs = [
        driver("check", "invocation", 0.0, 10.0),
        driver("all", "build", 0.0, 8.0),
        driver("relwithdebinfo", "env", 8.0, 9.0),
        job("a.cc", "compile", 1.0, 5.0),
        job("b.cc", "compile", 1.0, 5.0),
    ]
    summary = profile.summarize(jobs)

    leaf_types = {s.type for s in summary.leaves}
    container_types = {s.type for s in summary.containers}
    check("compile" in leaf_types and "env" in leaf_types, "real work is a leaf, whatever its origin")
    check(container_types == {"invocation", "build"}, f"only enclosing spans are containers, got {container_types}")
    check(not (leaf_types & container_types), "no type appears in both tables")

    total = summary.leaves[-1]
    check(total.type == "all leaves", "the leaf table ends with its own total")
    check(total.count == 3, f"which counts only leaves, got {total.count}")
    check(abs(total.total_s - 9.0) < 1e-9, f"summing 4+4+1 leaf seconds, got {total.total_s}")
    check(abs(total.span_s - 5.0) < 1e-9, f"spanning 1..5 plus 8..9, got {total.span_s}")
    check(summary.count == 5, "the header count still covers every job")


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
    """Fanned-out work must show a large sum against a small span — that gap is the parallelism."""
    jobs = [job(f"tu{i}", "compile", i * 0.1, i * 0.1 + 1.0) for i in range(8)]
    jobs.append(job("suite", "test", 5.0, 7.0))
    summary = profile.summarize(jobs)

    compile_row = next(s for s in summary.leaves if s.type == "compile")
    check(compile_row.count == 8, "every job of a type is counted")
    check(abs(compile_row.total_s - 8.0) < 1e-9, f"sum adds each job's duration, got {compile_row.total_s}")
    check(abs(compile_row.span_s - 1.7) < 1e-9, f"span counts the overlap once, got {compile_row.span_s}")
    check(compile_row.parallelism > 4.0, "so their ratio reports the fan-out")

    serial_row = next(s for s in summary.leaves if s.type == "test")
    check(abs(serial_row.parallelism - 1.0) < 1e-9, "a lone job is exactly 1.0x")

    check(summary.leaves[0].total_s >= summary.leaves[1].total_s, "types are ordered heaviest first")
    check(summary.leaves[-1].type == "all leaves", "and the total lands last, where the printer expects it")

    empty = profile.summarize([])
    check(empty.count == 0 and not empty.leaves and not empty.containers,
          "an empty profile summarizes to nothing at all")


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
    test_classify_finds_containers,
    test_concurrent_fan_out_never_contains_itself,
    test_driver_depth_follows_the_call_stack,
    test_adjacent_siblings_stay_siblings,
    test_split_summary_does_not_double_count,
    test_union_span_counts_overlap_once,
    test_summary_separates_work_from_wall_clock,
    test_chrome_round_trip,
    test_trace_separates_driver_from_fan_out,
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
