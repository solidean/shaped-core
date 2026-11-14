"""Reads clean-core's `.ccrec` recordings in Python, so a recording made once can be queried by any number of scripts.

    import ccrec
    rec = ccrec.load("run.ccrec")            # FormatError for any format version but this reader's own
    a = ccrec.analyze(rec)                    # scopes, values, stats, logs, samples, off-CPU stretches
    busy = ccrec.concurrency(a, ["task"])     # threads inside "task", over the whole recording

The decoding has the standard library alone; `to_numpy` imports numpy only when called.
libs/base/clean-core/docs/systems/recording-formats.md has how to run it.
"""

from .query import (
    Analysis,
    AsyncScopeRow,
    Concurrency,
    GapRow,
    LogRow,
    MarkerRow,
    RelationRow,
    SampleRow,
    ScopeInterval,
    StatRow,
    SummaryRow,
    ThreadRunRow,
    ValueRow,
    analyze,
    columns,
    concurrency,
    to_numpy,
)
from .reader import (
    VERSION,
    Block,
    Category,
    Desc,
    Domain,
    Event,
    EventKind,
    Field,
    FormatError,
    Level,
    Module,
    Recording,
    RelationType,
    Thread,
    TypeCode,
    Unit,
    load,
)

__all__ = [name for name in dir() if not name.startswith("_")]
