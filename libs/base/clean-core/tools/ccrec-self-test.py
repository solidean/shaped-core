#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///

"""Self-test for the `ccrec` reader: kept in step with the C++ format by a round trip.

    uv run libs/base/clean-core/tools/ccrec-self-test.py [-v]                       # runs the C++ half through dev.py first
    uv run libs/base/clean-core/tools/ccrec-self-test.py --fixture-dir <dir> [-v]   # reads what the C++ half already wrote

The C++ half is clean-core-test's "the Python reader's fixture" test, which records every event kind and payload type.
It writes the recording and a manifest of what the C++ loader reads back into `CC_REC_READER_FIXTURE_DIR`.
This script reads the same file and must arrive at the same manifest, byte for byte.
It also checks the fixture's known content through the query layer, and the reader's tables against the C++ headers.
`dev.py check` runs it as the `ccrec-reader` gate.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import traceback
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import ccrec  # noqa: E402
from ccrec import reader  # noqa: E402

ROOT = HERE.parents[3]
RECORD_SRC = HERE.parent / "src" / "clean-core" / "record"
FIXTURE_TEST = "the Python reader's fixture round-trips through the C++ loader"

VERBOSE = False


class Failure(Exception):
    pass


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def expect_eq(got, want, what: str) -> None:
    if got != want:
        raise Failure(f"{what}: got {got!r}, want {want!r}")


#
# The reader's tables against the C++ headers
#


def _enumerators(text: str, qualified: str) -> list[str]:
    m = re.search(r"enum class " + re.escape(qualified) + r"\s*:\s*[\w:]+\s*\{(.*?)\};", text, re.S)
    if m is None:
        raise Failure(f"no `enum class {qualified}` found")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    names = [n.strip().split("=")[0].strip() for n in body.split(",")]
    return [n for n in names if n and n != "count"]


def test_tables_match_the_headers() -> None:
    serialize_hh = (RECORD_SRC / "serialize.hh").read_text(encoding="utf-8")
    m = re.search(r"serialized_version\s*=\s*(\d+)\s*;", serialize_hh)
    expect(m is not None, "serialize.hh declares no serialized_version")
    expect_eq(reader.VERSION, int(m.group(1)), "reader.VERSION against cc::rec::serialized_version")

    sizes = {}
    for path in (RECORD_SRC / "impl" / "serialized_format.hh", RECORD_SRC / "writer.hh"):
        for name, size in re.findall(r"static_assert\(sizeof\((\w+)\)\s*==\s*(\d+)", path.read_text(encoding="utf-8")):
            sizes[name] = int(size)
    expect_eq(sizes, reader.STRUCT_SIZES, "struct sizes against the static_asserts")

    fwd = (RECORD_SRC / "fwd.hh").read_text(encoding="utf-8")
    for qualified, py in (("cc::rec::event_kind", ccrec.EventKind), ("cc::rec::level", ccrec.Level),
                          ("cc::rec::category", ccrec.Category), ("cc::rec::type_code", ccrec.TypeCode),
                          ("cc::rec::aggregation", reader.Aggregation), ("cc::rec::axis_scale", reader.AxisScale)):
        expect_eq([m.name for m in py], _enumerators(fwd, qualified), f"{py.__name__} against {qualified}")

    writer_hh = (RECORD_SRC / "writer.hh").read_text(encoding="utf-8")
    flags = dict(re.findall(r"u16 (flag_\w+) = (1 << \d+|0);", writer_hh))
    expect_eq(flags.get("flag_has_end_cycles"), "1 << 0", "flag_has_end_cycles")
    expect_eq(flags.get("flag_has_stacktrace"), "1 << 1", "flag_has_stacktrace")
    expect_eq(flags.get("flag_payload_pinned"), "1 << 2", "flag_payload_pinned")
    expect_eq(flags.get("flag_truncated"), "1 << 3", "flag_truncated")


#
# The manifest, as the reader sees the file
#


def _hex(value: str | bytes) -> str:
    raw = value.encode("utf-8", errors="surrogateescape") if isinstance(value, str) else value
    return raw.hex()


def _bits(v: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def _field_value(f: ccrec.Field, value) -> str:
    t = ccrec.TypeCode
    if f.type in (t.cstring, t.inline_text):
        return "-" if value is None else _hex(value)
    if f.type == t.pinned_bytes:
        return _hex(value)
    if f.type == t.u64_array:
        return "[" + "".join(f"{v}," for v in value) + "]"
    if f.type == t.desc_ref:
        return "-" if value is None else _hex(value.name)
    if f.type in (t.f32_, t.f64_):
        return "-" if value is None else str(_bits(float(value)))
    if value is None or f.type == t.none or not isinstance(f.type, t):
        return "-"
    return str(int(value))


def manifest_of(rec: ccrec.Recording, a: ccrec.Analysis) -> list[str]:
    """The same lines the C++ test's `manifest_of` writes, from this reader's decoding."""
    out = ["ccrec-manifest\t1", f"header\t{rec.version}\t{_bits(rec.cycles_per_second)}\t{int(rec.is_truncated)}"]
    for b in rec.blocks:
        out.append(f"block\t{b.thread.index}\t{b.thread.tid}\t{b.chunk_seq}\t{b.layer}\t{b.base_cycles}\t"
                   f"{_bits(b.base_wall_secs)}\t{b.seal_cycles}\t{_bits(b.seal_wall_secs)}\t{_hex(b.thread.name)}\t{len(b)}")
        for e in b:
            d = e.desc
            out.append(f"event\t{e.cycles}\t{int(e.kind)}\t{int(d.level)}\t{e.flags}\t{e.core}\t{e.payload.nbytes}\t"
                       f"{_hex(d.domain_name)}\t{_hex(d.file)}\t{d.line}\t{_hex(e.name)}")
            for f in e.payload_fields:
                out.append(f"field\t{_hex(f.name)}\t{int(f.type)}\t{_field_value(f, e.field(f.name))}")

    # recording::scopes() knows nothing of preambles, so the scopes only a preamble names are left out here.
    scopes = [s for s in a.scopes if s.begin_known]
    scopes.sort(key=lambda s: (s.begin_cycles, s.depth))
    for s in scopes:
        end = s.end_cycles if s.end_cycles is not None else s.begin_cycles
        out.append(f"scope\t{s.thread.tid}\t{s.begin_cycles}\t{end}\t{s.depth}\t{int(s.is_open)}\t{_hex(s.name)}")

    for log in sorted(a.logs, key=lambda r: r.cycles):
        out.append(f"message\t{_hex(log.text)}")
    for rel in sorted(a.relations, key=lambda r: r.cycles):
        out.append(f"relation\t{_hex(rel.type.name if rel.type else '')}\t" + "".join(f"{m}," for m in rel.members))
    return out


def test_manifest_matches(rec: ccrec.Recording, a: ccrec.Analysis, manifest: Path) -> None:
    want = manifest.read_text(encoding="utf-8").splitlines()
    got = manifest_of(rec, a)
    for i, (g, w) in enumerate(zip(got, want)):
        if g != w:
            raise Failure(f"manifest line {i + 1} differs from the C++ loader's:\n  reader: {g}\n  C++:    {w}")
    expect_eq(len(got), len(want), "manifest line count")


#
# The fixture's known content, through the query layer
#


def _thread(rec: ccrec.Recording, name: str) -> ccrec.Thread:
    found = [t for t in rec.threads if t.name == name]
    expect(len(found) == 1, f"one thread named {name!r}, found {len(found)}")
    return found[0]


def test_fixture_content(rec: ccrec.Recording, a: ccrec.Analysis) -> None:
    expect(not rec.is_truncated, "the fixture is not truncated")
    expect(rec.cycles_per_second > 0, "a cycle rate was measured")
    main = _thread(rec, "reader-main")
    workers = [_thread(rec, "reader-worker-0"), _thread(rec, "reader-worker-1")]

    # one value of every payload type
    values = {v.name: v for v in a.values if v.thread == main}
    want = {
        "v_bool": True, "v_i8": -5, "v_i16": -300, "v_i32": -70000, "v_i64": -1234567890123, "v_u8": 200,
        "v_u16": 65000, "v_u32": 4000000000, "v_u64": 0xFEDCBA9876543210, "v_f32": 1.5, "v_f64": -0.1,
        "v_ptr": 0x1234, "v_literal": "a literal", "v_text": "runtime text", "named-7": 7,
    }
    for name, value in want.items():
        expect(name in values, f"a value named {name!r}")
        expect_eq(values[name].value, value, f"value {name!r}")
        expect_eq(type(values[name].value), type(value), f"type of value {name!r}")
    expect_eq(values["v_literal"].desc.fields[0].type, ccrec.TypeCode.cstring, "a literal's field type")
    expect_eq(values["v_text"].desc.fields[0].type, ccrec.TypeCode.inline_text, "a runtime string's field type")
    expect_eq(values["named-7"].desc.name, "", "a runtime-named site's descriptor name")
    expect_eq(values["fixture.blob"].value, bytes((i * 7) & 0xFF for i in range(300)), "the pinned bytes")
    expect_eq(values["fixture.blob"].fields["size"], 300, "the pinned size")

    fixture = a.scopes_named("fixture")
    expect_eq(len(fixture), 1, "one fixture scope")
    for name in want:
        expect_eq(values[name].scope, fixture[0].index, f"{name!r} attributed to the fixture scope")

    # stats, markers, logs
    stats = {s.name: s for s in a.stats}
    expect_eq((stats["queue_depth"].value, stats["queue_depth"].accumulate), (3.0, False), "the snapshot")
    expect_eq((stats["bytes"].value, stats["bytes"].accumulate), (4096.0, True), "the accumulate")
    expect_eq((stats["bytes"].unit.symbol, stats["bytes"].unit.prefix_base), ("B", 1024), "the bytes unit, by value")
    expect(stats["queue_depth"].unit is not None, "a snapshot carries its unit")

    markers = Counter(m.name for m in a.markers)
    expect_eq((markers["fixture-mark"], markers["in-async"], markers["filler"]), (1, 1, 4000), "marker counts")
    filler = [m for m in a.markers if m.name == "filler"]
    expect(all(a.scope_path(m.scope) == "worker/task/leaf" for m in filler), "every filler sits in worker/task/leaf")

    expect_eq([(l.level, l.text) for l in a.logs],
              [(ccrec.Level.info, "plain message"), (ccrec.Level.warning, "took 2 tries"),
               (ccrec.Level.error, "fixture error 7")], "the log messages")
    expect(all(l.desc.domain_name == "default" for l in a.logs), "a log's domain")
    error = a.logs[2]
    if error.frames is not None:
        expect(len(error.frames) > 0, "a captured stack has frames")

    # relations and the async scope
    expect_eq(len(a.relations), 1, "one relation")
    expect_eq((a.relations[0].type.name, a.relations[0].members), ("parent_of", (0x1001, 0x1002, 0x1003)), "the relation")
    expect_eq(len(a.async_scopes), 1, "one async scope")
    s = a.async_scopes[0]
    expect_eq((s.name, s.trace, s.begin_thread, s.end_thread), ("fixture-async", 0x2002, main, main), "the async scope")
    in_async = next(m for m in a.markers if m.name == "in-async")
    expect(s.begin_cycles < in_async.cycles < s.end_cycles, "the async scope brackets what was recorded under it")
    expect_eq(in_async.trace, 0x2002, "the trace in effect inside the async scope")

    # nesting on the workers
    for w in workers:
        mine = [x for x in a.scopes if x.thread == w]
        expect_eq(Counter((x.name, x.depth) for x in mine), Counter({("worker", 0): 1, ("task", 1): 3, ("leaf", 2): 3}),
                  f"{w.name}'s scopes")
        for x in mine:
            expect(not x.is_open and x.duration_cycles > 0, f"{w.name}'s {x.name} closed after it opened")
            if x.parent >= 0:
                p = a.scopes[x.parent]
                expect(p.depth == x.depth - 1 and p.begin_cycles < x.begin_cycles and x.end_cycles < p.end_cycles,
                       f"{w.name}'s {x.name} nests inside its parent {p.name}")
        tasks = sorted((x for x in mine if x.name == "task"), key=lambda x: x.begin_cycles)
        indices = [v for v in a.values if v.thread == w and v.name == "task_index"]
        expect_eq([v.value for v in indices], [0, 1, 2], f"{w.name}'s task_index values")
        expect_eq([v.scope for v in indices], [t.index for t in tasks], f"{w.name}'s task_index scopes")

    # the rotated chunk's preamble names the three open scopes
    w0 = workers[0]
    blocks = rec.blocks_of_thread(w0.index)
    expect(len(blocks) >= 2, "the filler rotated worker 0's chunk")
    first = next(iter(blocks[1]))
    expect_eq(first.kind, ccrec.EventKind.stream_state, "a chunk opens with its preamble")
    expect_eq((first.field("scope_depth"), first.field("named_scopes")), (3, 3), "the preamble's depth and names")
    expect_eq([first.field(f"scope{i}").name for i in range(3)], ["worker", "task", "leaf"], "the preamble's scopes")
    expect_eq(first.field("scope0").kind, ccrec.EventKind.scope_begin, "a preamble names the begin descriptors")

    # the sample and the off-CPU reading
    expect_eq(len(a.samples), 1, "one sample")
    sample = a.samples[0]
    expect_eq((sample.sampled_thread, sample.native_tid, sample.frames), (w0, 4242, (0x1000, 0x2000, 0x3000)), "the sample")
    expect_eq([a.scopes[i].name for i in sample.open_scopes], ["worker", "task", "leaf"], "the scopes open at the sample")
    expect(any(b.chunk_seq == sample.chunk_seq for b in blocks), "the sample's anchor names one of the thread's chunks")

    expect_eq(len(a.thread_runs), 1, "one thread_run")
    run = a.thread_runs[0]
    expect_eq((run.sampled_thread, run.interval, run.ran, run.in_scope), (w0, 1000, 250, True), "the thread_run")
    expect_eq(run.ran_ratio, 0.25, "the thread_run's ratio")
    expect_eq(a.off_cpu(), [run], "the off-CPU stretches")

    # concurrency: the two workers' scopes overlap by construction, and the fixture scope is one thread's
    c = ccrec.concurrency(a, ["worker"])
    expect_eq(c.peak, 2, "both workers were inside 'worker' at once")
    expect_eq(sum(c.cycles_at().values()), c.end_cycles - c.begin_cycles, "the counts cover the window")
    probes = sorted({x.begin_cycles for x in a.scopes} | {x.end_cycles for x in a.scopes if x.end_cycles})
    for t in probes:
        if not (c.begin_cycles <= t < c.end_cycles):
            continue
        brute = len({x.thread.index for x in a.scopes_named("worker") if x.contains(t)})
        step = [n for at, n in c.steps if at <= t][-1]
        expect_eq(step, brute, f"concurrency at {t}")
    nested = ccrec.concurrency(a, ["worker", "task", "leaf"])
    expect_eq(nested.peak, 2, "nested scopes of one thread count once")
    f = fixture[0]
    one = ccrec.concurrency(a, ["fixture"], f.begin_cycles, f.end_cycles)
    expect_eq(one.steps, [(f.begin_cycles, 1)], "the fixture scope is one thread throughout")

    # time
    for b in rec.blocks:
        for e in b:
            expect(e.wall_secs <= rec.dumped_at_wall_secs + 1.0, "an event's wall time is before the dump")
    expect(rec.secs(f.duration_cycles) > 0, "a scope's duration in seconds")


#
# Refusing what it cannot read
#


def test_compressed_summaries(data: bytes, a: ccrec.Analysis) -> None:
    expect(data[:8] == reader.COMPRESSED_MAGIC, "the C++ half writes the fixture compressed")
    size = struct.unpack_from("<Q", data, 8)[0]
    try:
        ccrec.Recording(data[:8] + struct.pack("<Q", size - 1) + data[16:])
    except ccrec.FormatError as error:
        expect("size" in str(error), f"a wrong size in the envelope: refused for the wrong reason: {error}")
    else:
        raise Failure("a wrong size in the envelope: read rather than refused")

    bytes_summaries = [s for s in a.summaries if s.name == "bytes"]
    expect_eq(len(bytes_summaries), 1, "the accumulated stat's summary")
    s = bytes_summaries[0]
    expect_eq((s.count, s.sum, s.last), (1, 4096.0, 4096.0), "what the summary folds")
    expect(s.folds == ccrec.EventKind.stat_accumulate, f"a summary names the kind it folds, got {s.folds}")


def test_refusals(data: bytes) -> None:
    def refused(bad: bytes, what: str, needle: str) -> None:
        try:
            ccrec.Recording(bad)
        except ccrec.FormatError as error:
            expect(needle in str(error), f"{what}: refused for the wrong reason: {error}")
            return
        raise Failure(f"{what}: read rather than refused")

    refused(b"", "an empty file", "too short")
    refused(b"X" + data[1:], "a bad magic", "bad magic")
    for version in (reader.VERSION - 1, reader.VERSION + 1):
        refused(data[:8] + struct.pack("<I", version) + data[12:], f"version {version}", "version")
    refused(data[: len(data) // 2], "a truncated file", "malformed")

    # An event naming a descriptor past the table: the first event's desc slot, in the first block.
    rec = ccrec.Recording(data)
    at = rec.blocks[0].file_offset
    refused(data[:at] + struct.pack("<Q", len(rec.descs)) + data[at + 8:], "a dangling descriptor index", "descriptor")


#
# Driver
#


def produce_fixture(directory: Path) -> None:
    """Runs the C++ half through dev.py, which builds clean-core-test first."""
    env = dict(os.environ, CC_REC_READER_FIXTURE_DIR=str(directory))
    proc = subprocess.run(["uv", "run", str(ROOT / "dev.py"), "test", FIXTURE_TEST], cwd=ROOT, env=env,
                          capture_output=not VERBOSE, text=True)
    if proc.returncode != 0:
        if not VERBOSE:
            sys.stdout.write(proc.stdout)
            sys.stderr.write(proc.stderr)
        raise Failure(f"the C++ half failed (exit {proc.returncode})")


def main() -> int:
    global VERBOSE
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fixture-dir", type=Path, help="where the C++ half wrote fixture.ccrec and fixture.manifest")
    parser.add_argument("-v", action="store_true", dest="verbose")
    args = parser.parse_args()
    VERBOSE = args.verbose

    results: list[tuple[str, BaseException | None]] = []

    def run(name: str, fn, *fn_args) -> None:
        try:
            fn(*fn_args)
            results.append((name, None))
            if VERBOSE:
                print(f"  ok    {name}")
        except Exception as error:  # noqa: BLE001 — a self-test reports every failure rather than stopping at the first
            results.append((name, error))
            print(f"  FAIL  {name}: {error}")
            if VERBOSE and not isinstance(error, Failure):
                traceback.print_exc()

    run("the reader's tables match the C++ headers", test_tables_match_the_headers)

    with tempfile.TemporaryDirectory(prefix="ccrec-self-test-") as tmp:
        directory = args.fixture_dir
        if directory is None:
            directory = Path(tmp)
            run("the C++ half writes the fixture", produce_fixture, directory)

        recording_path = directory / "fixture.ccrec"
        manifest_path = directory / "fixture.manifest"
        if recording_path.is_file() and manifest_path.is_file():
            data = recording_path.read_bytes()
            rec = ccrec.Recording(data)
            a = ccrec.analyze(rec)
            run("the reader reads the C++ loader's manifest", test_manifest_matches, rec, a, manifest_path)
            run("the fixture's content reads back through the query layer", test_fixture_content, rec, a)
            run("the fixture is compressed, and its summaries read as summaries", test_compressed_summaries, data, a)
            # The refusals edit the recording's own bytes, so they take it out of its envelope first.
            run("a file it cannot read is refused", test_refusals, reader._decompressed(memoryview(data)))
        else:
            results.append(("the fixture exists", Failure(f"no fixture.ccrec and fixture.manifest in {directory}")))
            print(f"  FAIL  no fixture.ccrec and fixture.manifest in {directory}")

    failures = sum(1 for _, error in results if error is not None)
    print(f"ccrec-self-test: {len(results) - failures} of {len(results)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
