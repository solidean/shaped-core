"""A small query layer over a decoded recording, for performance analysis after the fact.

`analyze(recording)` walks every thread's stream once and returns an `Analysis`.
It holds the scope intervals with their nesting, and the values, stats, markers and logs, each attributed to the scope it was recorded inside.
It also holds the samples, the off-CPU readings (`record.thread_run`), the async scopes and the trace relations.
`concurrency()` answers how many threads were inside a set of scopes over a time window.

Nothing here knows a scope's name: every query takes the names it is about.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass, field, fields as dc_fields
from typing import Iterable, Sequence

from .reader import (
    FLAG_HAS_STACKTRACE,
    FLAG_EVENT_TRUNCATED,
    SAMPLE_UNKNOWN_THREAD,
    Desc,
    Event,
    EventKind,
    Level,
    Recording,
    RelationType,
    Thread,
    Unit,
)

#: The value site every `record.thread_run` reading is written through (sampling.hh).
THREAD_RUN_NAME = "record.thread_run"

#: The value site a log message's captured stack is written through, right after the message (log.cc).
STACKTRACE_NAME = "record.stacktrace"


@dataclass(slots=True)
class ScopeInterval:
    """One profiling scope, from its begin to its end on one thread.

    `end_cycles` is None for a scope the recording never saw close.
    `begin_known` is False for a scope only a chunk preamble names: it was open when the window starts, so `begin_cycles` is that preamble's time and the real start is earlier.
    `name` is empty for a preamble scope deeper than the three slots a preamble names.
    """

    index: int
    name: str
    desc: Desc | None
    thread: Thread
    layer: int
    begin_cycles: int
    end_cycles: int | None
    depth: int
    parent: int
    begin_known: bool = True

    @property
    def is_open(self) -> bool:
        return self.end_cycles is None

    @property
    def duration_cycles(self) -> int | None:
        return None if self.end_cycles is None else self.end_cycles - self.begin_cycles

    def contains(self, cycles: int) -> bool:
        return self.begin_cycles <= cycles and (self.end_cycles is None or cycles < self.end_cycles)


@dataclass(slots=True)
class Attributed:
    """What every recorded row shares: where and when it was recorded, the innermost scope open around it, and the trace and owner in effect."""

    name: str
    desc: Desc
    thread: Thread
    layer: int
    cycles: int
    scope: int
    trace: int
    owner: int


@dataclass(slots=True)
class ValueRow(Attributed):
    """A `value` event (CC_RECORD, CC_RECORD_NAMED, CC_RECORD_PINNED, and the recorder's own value sites), its fields decoded."""

    fields: dict[str, object] = field(default_factory=dict)

    @property
    def value(self):
        return self.fields.get("value")


@dataclass(slots=True)
class StatRow(Attributed):
    """A stat_snapshot or stat_accumulate reading; `accumulate` says which, since summing snapshots means nothing."""

    value: float = 0.0
    unit: Unit | None = None
    accumulate: bool = False


@dataclass(slots=True)
class MarkerRow(Attributed):
    """A marker: this code ran."""


@dataclass(slots=True)
class LogRow(Attributed):
    """A log message as it would print, and the stack its domain captured with it, if any."""

    level: Level | int = Level.info
    text: str = ""
    truncated: bool = False
    frames: tuple[int, ...] | None = None


@dataclass(slots=True)
class SampleRow:
    """A sampler's stack sample of one thread: return addresses, innermost first, never symbolized here.

    `sampled_thread` is None for a thread the recorder has no stream for.
    `open_scopes` are the sampled thread's scopes open at the sample's time, outermost first, which name the frames a sample stops short of.
    """

    cycles: int
    recorded_on: Thread
    sampled_thread: Thread | None
    thread_index: int
    chunk_seq: int
    chunk_offset: int
    native_tid: int
    frames: tuple[int, ...]
    truncated: bool
    open_scopes: list[int] = field(default_factory=list)


@dataclass(slots=True)
class ThreadRunRow:
    """How much of a stretch a thread ran on a CPU: `ran` of `end_cycles - begin_cycles`, both in timestamp ticks.

    `in_scope` is True when the thread had a recorded scope open at the stretch's end: off the CPU inside its work, rather than idle.
    """

    thread_index: int
    sampled_thread: Thread | None
    native_tid: int
    begin_cycles: int
    end_cycles: int
    ran: int
    in_scope: bool
    recorded_on: Thread

    @property
    def interval(self) -> int:
        return self.end_cycles - self.begin_cycles

    @property
    def ran_ratio(self) -> float:
        return self.ran / self.interval if self.interval > 0 else 0.0

    @property
    def off_cpu_cycles(self) -> int:
        return max(0, self.interval - self.ran)


@dataclass(slots=True)
class AsyncScopeRow:
    """An async scope object's life, from its begin to its end, which may be on different threads."""

    name: str
    trace: int
    begin_cycles: int
    end_cycles: int | None
    begin_thread: Thread
    end_thread: Thread | None


@dataclass(slots=True)
class RelationRow:
    """Some trace ids are related: the first member is the subject, the rest are objects."""

    type: RelationType | None
    members: tuple[int, ...]
    cycles: int
    thread: Thread


@dataclass(slots=True)
class GapRow:
    """Events a thread could not record (`gap`), or that a decimation dropped (`dropped_span`)."""

    kind: EventKind | int
    thread: Thread
    cycles: int
    events: int
    begin_cycles: int
    end_cycles: int


@dataclass
class Analysis:
    recording: Recording
    scopes: list[ScopeInterval] = field(default_factory=list)
    values: list[ValueRow] = field(default_factory=list)
    stats: list[StatRow] = field(default_factory=list)
    markers: list[MarkerRow] = field(default_factory=list)
    logs: list[LogRow] = field(default_factory=list)
    samples: list[SampleRow] = field(default_factory=list)
    thread_runs: list[ThreadRunRow] = field(default_factory=list)
    async_scopes: list[AsyncScopeRow] = field(default_factory=list)
    relations: list[RelationRow] = field(default_factory=list)
    gaps: list[GapRow] = field(default_factory=list)

    #: The cycles of the last event in the recording, which an open scope is taken to last until.
    last_cycles: int = 0

    # per thread index, the scopes sorted by begin, for the time lookups
    _by_thread: dict[int, tuple[list[int], list[int]]] = field(default_factory=dict, repr=False)

    def scopes_named(self, *names: str) -> list[ScopeInterval]:
        wanted = set(names)
        return [s for s in self.scopes if s.name in wanted]

    def children(self, scope: int) -> list[ScopeInterval]:
        return [s for s in self.scopes if s.parent == scope]

    def ancestors(self, scope: int) -> list[ScopeInterval]:
        """The scope's enclosing scopes, outermost first; empty for a scope at the outside."""
        out = []
        at = self.scopes[scope].parent if scope >= 0 else -1
        while at >= 0:
            out.append(self.scopes[at])
            at = self.scopes[at].parent
        return out[::-1]

    def scope_path(self, scope: int) -> str:
        """The scope's name prefixed by its ancestors', `outer/inner`, for grouping."""
        if scope < 0:
            return ""
        return "/".join(s.name for s in (*self.ancestors(scope), self.scopes[scope]))

    def open_scopes_at(self, thread_index: int, cycles: int) -> list[int]:
        """The scopes open on a thread at a time, outermost first.

        The latest scope begun at or before `cycles` is found by bisection, and every scope open then is it or one of its ancestors, since scopes on one thread nest.
        """
        lane = self._by_thread.get(thread_index)
        if lane is None:
            return []
        begins, order = lane
        i = bisect.bisect_right(begins, cycles) - 1
        if i < 0:
            return []
        out = []
        at = order[i]
        while at >= 0:
            s = self.scopes[at]
            if s.contains(cycles):
                out.append(at)
            at = s.parent
        return out[::-1]

    def total_cycles_by_name(self, *, threads: Iterable[int] | None = None) -> dict[str, int]:
        """Summed closed-scope durations per name; nested scopes of one name count once per level."""
        keep = None if threads is None else set(threads)
        out: dict[str, int] = {}
        for s in self.scopes:
            if s.end_cycles is None or (keep is not None and s.thread.index not in keep):
                continue
            out[s.name] = out.get(s.name, 0) + s.end_cycles - s.begin_cycles
        return out

    def off_cpu(self, *, max_ran_ratio: float = 0.5) -> list[ThreadRunRow]:
        """The thread_run stretches a thread mostly spent off the CPU, by the threshold babel::chrome_trace draws its off-CPU lanes at."""
        return [r for r in self.thread_runs if r.interval > 0 and r.ran_ratio < max_ran_ratio]


def _stream_key(e: Event) -> tuple[int, int]:
    return (e.block.thread.index, e.block.layer)


def analyze(recording: Recording) -> Analysis:
    """Walks every thread's stream in order and builds the tables."""
    a = Analysis(recording)

    # One stream per thread and listener layer, each ordered by its strictly increasing timestamps.
    streams: dict[tuple[int, int], list[tuple[int, int, Event]]] = {}
    order = 0
    for e in recording.events():
        streams.setdefault(_stream_key(e), []).append((e.cycles, order, e))
        order += 1
        if e.cycles > a.last_cycles:
            a.last_cycles = e.cycles

    for key in sorted(streams):
        stream = streams[key]
        stream.sort(key=lambda t: (t[0], t[1]))
        stack: list[int] = []
        trace = 0
        owner = 0
        last_log: LogRow | None = None

        for _, _, e in stream:
            kind = e.kind
            thread = e.block.thread
            layer = e.block.layer
            scope = stack[-1] if stack else -1

            if kind == EventKind.scope_begin:
                depth = e.field("depth") or 0
                a.scopes.append(ScopeInterval(len(a.scopes), e.name, e.desc, thread, layer, e.cycles, None, depth, scope))
                stack.append(len(a.scopes) - 1)
                last_log = None
                continue

            if kind == EventKind.scope_end:
                # Innermost first, by name and depth, as recording::scopes() matches; a preamble scope with no name
                # matches by depth alone.
                depth = e.field("depth") or 0
                name = e.name
                for i in range(len(stack) - 1, -1, -1):
                    s = a.scopes[stack[i]]
                    if s.depth == depth and (s.name == name or (not s.begin_known and not s.name)):
                        s.end_cycles = e.cycles
                        del stack[i]
                        break
                last_log = None
                continue

            if kind == EventKind.stream_state:
                trace = e.field("trace") or 0
                owner = e.field("owner") or 0
                # A window that starts inside scopes knows them only from here.
                depth = e.field("scope_depth") or 0
                if not stack and depth > 0:
                    named = min(e.field("named_scopes") or 0, 3)
                    for d in range(depth):
                        desc = e.field(f"scope{d}") if d < named else None
                        parent = stack[-1] if stack else -1
                        a.scopes.append(ScopeInterval(len(a.scopes), desc.name if desc else "", desc, thread, layer,
                                                      e.cycles, None, d, parent, begin_known=False))
                        stack.append(len(a.scopes) - 1)
                continue

            if kind == EventKind.ambient_changed:
                trace = e.field("trace") or 0
                owner = e.field("owner") or 0
                continue

            common = dict(name=e.name, desc=e.desc, thread=thread, layer=layer, cycles=e.cycles, scope=scope,
                          trace=trace, owner=owner)

            if kind == EventKind.value:
                fields = e.fields()
                if e.name == THREAD_RUN_NAME:
                    index = fields.get("thread_index")
                    interval = fields.get("interval") or 0
                    a.thread_runs.append(ThreadRunRow(
                        thread_index=index, sampled_thread=recording.thread_by_index(index),
                        native_tid=fields.get("native_tid") or 0, begin_cycles=e.cycles - interval, end_cycles=e.cycles,
                        ran=fields.get("ran") or 0, in_scope=bool(fields.get("in_scope")), recorded_on=thread))
                if e.name == STACKTRACE_NAME and last_log is not None and (e.flags & FLAG_HAS_STACKTRACE):
                    last_log.frames = tuple(fields.get("frames") or ())
                a.values.append(ValueRow(**common, fields=fields))
                last_log = None
                continue

            last_log = None

            if kind in (EventKind.stat_snapshot, EventKind.stat_accumulate):
                a.stats.append(StatRow(**common, value=e.field("value"), unit=e.desc.unit,
                                       accumulate=kind == EventKind.stat_accumulate))
            elif kind == EventKind.marker:
                a.markers.append(MarkerRow(**common))
            elif kind == EventKind.log:
                last_log = LogRow(**common, level=e.level, text=e.text, truncated=e.is_truncated)
                a.logs.append(last_log)
            elif kind == EventKind.sample:
                index = e.field("thread_index")
                a.samples.append(SampleRow(
                    cycles=e.cycles, recorded_on=thread,
                    sampled_thread=None if index == SAMPLE_UNKNOWN_THREAD else recording.thread_by_index(index),
                    thread_index=index, chunk_seq=e.field("chunk_seq"), chunk_offset=e.field("chunk_offset"),
                    native_tid=e.field("native_tid"), frames=tuple(e.field("frames") or ()),
                    truncated=(e.flags & FLAG_EVENT_TRUNCATED) != 0))
            elif kind == EventKind.trace_relation:
                a.relations.append(RelationRow(e.desc.relation, tuple(e.field("members") or ()), e.cycles, thread))
            elif kind in (EventKind.gap, EventKind.dropped_span):
                a.gaps.append(GapRow(kind, thread, e.cycles, e.field("events") or 0, e.field("begin_cycles") or 0,
                                     e.field("end_cycles") or 0))

    # An async scope's two ends may sit on different threads, so they pair in global time order rather than per stream.
    _pair_async_scopes(a, recording)

    # The per-thread lookup the time queries bisect over.
    lanes: dict[int, list[ScopeInterval]] = {}
    for s in a.scopes:
        lanes.setdefault(s.thread.index, []).append(s)
    for index, lane in lanes.items():
        lane.sort(key=lambda s: (s.begin_cycles, s.depth))
        a._by_thread[index] = ([s.begin_cycles for s in lane], [s.index for s in lane])

    for row in a.samples:
        if row.sampled_thread is not None:
            row.open_scopes = a.open_scopes_at(row.thread_index, row.cycles)

    return a


def _pair_async_scopes(a: Analysis, recording: Recording) -> None:
    a.async_scopes.clear()
    pending: dict[tuple[str, int], list[AsyncScopeRow]] = {}
    for e in recording.sorted_events():
        if e.kind == EventKind.async_scope_begin:
            row = AsyncScopeRow(e.name, e.field("trace") or 0, e.cycles, None, e.block.thread, None)
            pending.setdefault((row.name, row.trace), []).append(row)
            a.async_scopes.append(row)
        elif e.kind == EventKind.async_scope_end:
            rows = pending.get((e.name, e.field("trace") or 0))
            if rows:
                row = rows.pop()
                row.end_cycles = e.cycles
                row.end_thread = e.block.thread


@dataclass
class Concurrency:
    """How many threads were inside the chosen scopes, as a step function over a window.

    `steps` are `(cycles, count)` pairs: from each `cycles` until the next, `count` threads were inside.
    """

    begin_cycles: int
    end_cycles: int
    steps: list[tuple[int, int]]

    def cycles_at(self) -> dict[int, int]:
        """How long the window spent at each count, in cycles."""
        out: dict[int, int] = {}
        bounds = [c for c, _ in self.steps] + [self.end_cycles]
        for (c, n), nxt in zip(self.steps, bounds[1:]):
            out[n] = out.get(n, 0) + nxt - c
        return out

    @property
    def mean(self) -> float:
        span = self.end_cycles - self.begin_cycles
        if span <= 0:
            return 0.0
        return sum(n * c for n, c in self.cycles_at().items()) / span

    @property
    def peak(self) -> int:
        return max((n for _, n in self.steps), default=0)


def concurrency(analysis: Analysis, names: Iterable[str], begin_cycles: int | None = None,
                end_cycles: int | None = None, *, threads: Iterable[int] | None = None) -> Concurrency:
    """For a time window, how many threads were inside at least one scope named in `names`.

    A thread inside two such scopes at once counts once, so nesting and recursion do not inflate the count.
    An open scope is taken to last until the recording's last event.
    The window defaults to the first and last event of the recording.
    """
    wanted = set(names)
    keep = None if threads is None else set(threads)
    lo = begin_cycles if begin_cycles is not None else min((s.begin_cycles for s in analysis.scopes), default=0)
    hi = end_cycles if end_cycles is not None else analysis.last_cycles

    per_thread: dict[int, list[tuple[int, int]]] = {}
    for s in analysis.scopes:
        if s.name not in wanted or (keep is not None and s.thread.index not in keep):
            continue
        b = max(s.begin_cycles, lo)
        e = min(s.end_cycles if s.end_cycles is not None else analysis.last_cycles, hi)
        if e > b:
            per_thread.setdefault(s.thread.index, []).append((b, e))

    # Each thread's intervals merged first, which is what makes one thread count once.
    deltas: dict[int, int] = {}
    for spans in per_thread.values():
        spans.sort()
        cur_b, cur_e = spans[0]
        for b, e in spans[1:]:
            if b <= cur_e:
                cur_e = max(cur_e, e)
                continue
            deltas[cur_b] = deltas.get(cur_b, 0) + 1
            deltas[cur_e] = deltas.get(cur_e, 0) - 1
            cur_b, cur_e = b, e
        deltas[cur_b] = deltas.get(cur_b, 0) + 1
        deltas[cur_e] = deltas.get(cur_e, 0) - 1

    steps = [(lo, 0)]
    count = 0
    for at in sorted(deltas):
        count += deltas[at]
        if at == steps[-1][0]:
            steps[-1] = (at, count)
        elif count != steps[-1][1]:
            steps.append((at, count))
    if steps and steps[-1][0] >= hi and len(steps) > 1:
        steps.pop()
    return Concurrency(lo, hi, steps)


def columns(rows: Sequence[object], *names: str) -> dict[str, list]:
    """A table as columns: for each attribute name, the list of every row's value.

    With no names, every dataclass field of the first row; a column of Thread or Desc values stays objects.
    """
    if not rows:
        return {n: [] for n in names}
    if not names:
        names = tuple(f.name for f in dc_fields(rows[0]))
    return {n: [getattr(r, n) for r in rows] for n in names}


def to_numpy(rows: Sequence[object], *names: str):
    """The columns as numpy arrays, for a script that declares numpy; imported here, so the reader itself needs none."""
    import numpy as np

    return {n: np.asarray(v) for n, v in columns(rows, *names).items()}
