---
name: analyzing-recordings
description: Answer questions about a `.ccrec` recording — what a run, a test, a benchmark or a crash recorded — by writing a short Python script against clean-core's `ccrec` package. Covers where recordings come from, the CLI for a first look, and the query layer for everything else: counts by site, time by scope, stats, logs, threads, concurrency.
when_to_use: '".ccrec", "ccrec", "the recording", "what did the run record", "analyze the recording", "which scopes", "top sites by count", "why is the recording so large", "where did the time go", "how often is X called", "what was logged", "stats in the recording", "per-thread", "how busy were the threads", "crash recording", "run-logs ccrec", "compacted recording", "summary events"'
allowed-tools: Bash Read Write
---

## Getting a recording

- **Every test run already left one**: `build/<preset>/run-logs/<binary>.ccrec`, the whole run compacted to about a megabyte, compressed.
  It has the instrumentation alone, short scopes folded into summaries; start here before rerunning anything.
- `uv run dev.py test "<pattern>" --rec` keeps the run's recording beside the binary, `<binary>.ccrec`; dev.py prints its path and size.
  `--stop-after SECS` bounds the run, which bounds the file.
- `--sample` records too, with stack samples on top of the instrumented scopes.
- A crashed or hung test leaves its `.ccrec` in the `crashes/` folder dev.py names.
- `uv run dev.py benchmark <match> --rec <file>` records the whole benchmark run, warmup included.

## First look: the CLI

```bash
uv run libs/base/clean-core/tools/ccrec-cli.py summary <file>          # header, threads, event kinds per thread
uv run libs/base/clean-core/tools/ccrec-cli.py scopes <file> --top 20  # scope names by total time
uv run libs/base/clean-core/tools/ccrec-cli.py events <file> --thread 3 --limit 200
```

Anything the CLI does not answer is a script, never a hand count over `events` output.

## The script

Write it to the scratchpad, never into the repo, and run it with `uv run --script <file>.py`.
The package is standard library only, so the script declares no dependencies unless it wants numpy.

```python
# /// script
# requires-python = ">=3.14"
# dependencies = []
# ///
import sys
from collections import Counter
sys.path.insert(0, r"<shaped-core>/libs/base/clean-core/tools")
import ccrec

rec = ccrec.load(r"<file>.ccrec")          # raw events: rec.events(), rec.threads, rec.descs
a = ccrec.analyze(rec)                     # one pass into tables: a.scopes, a.summaries, a.stats, a.values, a.logs, a.samples, …

# what dominates the file: event count by kind, then by site
print(Counter(e.kind.name for e in rec.events()).most_common())
print(Counter((e.kind.name, e.name) for e in rec.events()).most_common(20))

# where the time went: total and count per scope name, collapsed scopes included
counts = a.counts_by_name()
for name, cycles in sorted(a.total_cycles_by_name().items(), key=lambda kv: -kv[1])[:20]:
    print(f"{rec.secs(cycles):8.3f} s  {counts.get(name, 0):7}  {name}")

# one scope's intervals, with their thread and nesting
for s in a.scopes_named("sg.command_list.submit")[:5]:
    print(s.thread.name, rec.secs(s.duration_cycles or 0), a.scope_path(s.index))

# stats summed per site, summarized lanes included; each row knows the scope it was recorded in
totals = Counter()
for st in a.stats:
    totals[st.name] += st.value
for m in a.summaries:
    if m.folds == ccrec.EventKind.stat_accumulate:
        totals[m.name] += m.sum
# logs, by level and text
print(Counter((l.level.name, l.desc.name) for l in a.logs).most_common(10))
```

The rest of the surface, each a one-liner on `a`:
- `children(i)`, `ancestors(i)` and `open_scopes_at(thread, cycles)` for scope nesting.
- `off_cpu()` for threads that mostly sat off the CPU.
- `ccrec.concurrency(a, ["name"], begin, end)` for how many threads were inside a scope over time.
- `ccrec.columns(rows)` and `ccrec.to_numpy(rows)` turn any table into columns.

[docs/systems/recording-formats.md](../../../libs/base/clean-core/docs/systems/recording-formats.md#reading-a-recording-in-python) is the reference.
The docstrings in [query.py](../../../libs/base/clean-core/tools/ccrec/query.py) hold every field.

## What trips a reading

- **A compacted recording folds what was short or frequent into summaries** (`a.summaries`, kind `summary`, the site's own name).
  A collapsed scope is no interval in `a.scopes`; `total_cycles_by_name` and `counts_by_name` add the summaries back, a hand loop over `a.scopes` does not.
- **Times are cycles.** `rec.secs(cycles)` converts; a scope still open at the end has `end_cycles is None`.
- **`total_cycles_by_name` is inclusive and summed over threads**, so nested scopes count twice and the column can exceed the run's wall time.
- **The clang development presets record opaque source sites**, so `desc.file` is empty and `desc.line` 0; group by `e.name`, which is still there.
- **`e.name` and `e.desc.name` differ for runtime-named sites** (`CC_RECORD_NAMED`); group by `e.name`.
- **The reader refuses any format version but its own** with `ccrec.FormatError`: a recording from an older build needs that build's reader, from its commit.
- **Count before you conclude.** A site that dominates the event count may cost nothing, and a rare scope may hold all the time: show both tables.
