"""Where a request's time went, kept in memory for the life of the server.

The page is local and every interaction is a round trip, so "the UI feels slow" is always a question about
one of a handful of routes.
Answering it by guessing is how a cache ends up on the thing that was already fast.

Nothing here is persisted.
The action log is the review's record of what changed; timings are a property of one process's run and belong
nowhere near it.

Two levels, because they answer different questions.
A **phase** is a named span inside one request — parsing the entries, rendering, building tokens — and is what
says which part to fix.
A **route** is the whole request, and is what says whether it is worth fixing at all.
"""

from __future__ import annotations

import sys
import threading
import time
from contextlib import contextmanager

# Per (route, phase) -> [count, total_ms, min_ms, max_ms], plus every sample for the percentiles.
# A review is read for minutes, not days, so the samples are kept whole rather than bucketed: a few thousand
# floats is nothing, and an exact p95 beats an approximate one when the question is "why did that click stall".
_stats: dict[tuple[str, str], list] = {}
_samples: dict[tuple[str, str], list[float]] = {}
_guard = threading.Lock()

# Printed per request when on, which is what `serve --timings` sets.
_echo = False


def set_echo(on: bool) -> None:
    global _echo
    _echo = on


def note(route: str, phase: str, ms: float) -> None:
    """Record one span.

    Called from the request thread, so it takes the lock and does nothing else.
    """
    key = (route, phase)
    with _guard:
        row = _stats.get(key)
        if row is None:
            _stats[key] = [1, ms, ms, ms]
            _samples[key] = [ms]
        else:
            row[0] += 1
            row[1] += ms
            row[2] = min(row[2], ms)
            row[3] = max(row[3], ms)
            _samples[key].append(ms)
    if _echo:
        # stderr rather than the log file: this is a thing you watch while clicking, not a thing you read later.
        print(f"[timing] {route:28} {phase:20} {ms:8.1f} ms", file=sys.stderr, flush=True)


@contextmanager
def span(route: str, phase: str):
    """Times a block and records it, whatever it raises.

    A request that failed still spent its time, and a phase that only reports on success hides exactly the
    slow paths worth finding.
    """
    started = time.perf_counter()
    try:
        yield
    finally:
        note(route, phase, (time.perf_counter() - started) * 1000.0)


def _percentile(sorted_samples: list[float], fraction: float) -> float:
    if not sorted_samples:
        return 0.0
    at = min(len(sorted_samples) - 1, int(fraction * len(sorted_samples)))
    return sorted_samples[at]


def report() -> dict:
    """Everything recorded so far, newest-heaviest first.

    Sorted by total time rather than by mean: the route worth fixing is the one the reader waits on most across
    a session, which a slow route nobody visits is not.
    """
    with _guard:
        rows = []
        for (route, phase), (count, total, low, high) in _stats.items():
            ordered = sorted(_samples[(route, phase)])
            rows.append({
                "route": route,
                "phase": phase,
                "count": count,
                "total_ms": round(total, 1),
                "mean_ms": round(total / count, 1),
                "min_ms": round(low, 1),
                "p50_ms": round(_percentile(ordered, 0.50), 1),
                "p95_ms": round(_percentile(ordered, 0.95), 1),
                "max_ms": round(high, 1),
            })
    rows.sort(key=lambda r: r["total_ms"], reverse=True)
    return {"rows": rows}


def reset() -> None:
    """Drops everything, so a measurement can start from a known point mid-session."""
    with _guard:
        _stats.clear()
        _samples.clear()
