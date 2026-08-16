# Measuring compile times

`dev.py check --profile` says which *phase* of a run is slow ([Profiling a run](building-and-testing.md#profiling-a-run)).
This is the other half: once you know compilation dominates, `dev.py compile-time` says *which headers and which translation units*.
`dev.py build --files` then lets you iterate on them without building the world.

[docs/notes/build-times.md](../notes/build-times.md) is the findings log these tools feed.

## The three questions

```bash
uv run dev.py compile-time headers "libs/base/clean-core/src/clean-core/container/*.hh"
uv run dev.py compile-time tu      "libs/base/clean-core/tests/container/*.cc"
uv run dev.py compile-time pch     "libs/graphics/shaped-graphics/src/**/*.cc"
```

**`headers`** compiles a synthetic TU whose entire body is `#include <that header>`, with the real flags of the target that owns it.
This measures the header's **transitive closure**, deliberately: a small header that pulls in the world is exactly what it is meant to expose.

**`tu`** compiles each real TU twice — as-is, and reduced to its preprocessor directives — so the two wall clocks bracket how much of the file is its includes.
Both halves run back to back on the same machine state, which is the point.
Runs vary by ~8 % (see [Runs vary](../notes/build-times.md#runs-vary-by-8)), so a ratio taken across two separate invocations would be swamped by it.

The `incl%` column is the answer:

```
      full   includes   incl%       own  TU
    1.576s     1.334s   84.6%    0.242s  .../dx12/tests/dx12-clear-test.cc
    1.772s     0.516s   29.1%    1.256s  .../clean-core/tests/container/vector-test.cc
```

A dx12 test is 85 % includes and a clean-core container test is 29 %, which is why they want opposite fixes.

**`pch`** compiles each real TU with the precompiled header its target is configured for, and again without it, and prints the ratio per TU and per target.
That is what a target's tier is currently worth; [precompiled-headers.md](precompiled-headers.md) is how to change one.
It builds the preset first, because the flags name a `.pch` the target's own build produces.

**`headers` and `tu` strip the PCH flags; only `pch` keeps them.**
That is a correctness requirement rather than tidiness.
A header measured against a closure that is already deserialized reads as ~0.03 s no matter what it contains — a plausible number rather than an error, which is the worst kind of wrong.
So `headers` and `tu` always measure parsing from source, and their numbers are comparable across a PCH and a `nopch-*` preset.

## Reading the output

Both modes write JSON (`.tmp/compile-time/<mode>-<preset>.json` unless `--out` says otherwise) and print a top-K table.
Every record carries the wall clock **and** a `-ftime-trace` breakdown:

| field | meaning |
|---|---|
| `wall_s` | end-to-end wall clock, the minimum across `--repeat` runs |
| `runs` | every repeat, so you can see the spread |
| `trace.frontend_s` / `backend_s` | parse and codegen |
| `trace.source_s` | time inside `#include`d files |
| `trace.ours_source_s` / `system_source_s` | **the split that decides whether include hygiene can pay at all** |
| `trace.include_count` | distinct files entered |
| `trace.headers` | every header entered, with its exclusive self time |
| `trace.headers_omitted` / `headers_omitted_s` | the sub-0.5 ms tail that was dropped, and what it summed to |

`ours_source_s` versus `system_source_s` is usually the first thing to look at.
On a dx12 test TU it is 0.32 s ours against 0.98 s system (MSVC STL, Windows SDK, `d3d12.h`), which caps what cleaning up our own headers can win.

## Exploring the results

```bash
uv run dev.py compile-time export .tmp/compile-time/all-*.json --csv-dir .tmp/compile-time
```

Flattens any number of reports into two **long-format** CSVs, which is the shape a pivot table and a dataframe both want.
Reach for this when you do not yet know the question — a report answers the questions someone already had, and a pivot answers the ones you find.

`records.csv` — one row per measured file:

| column | |
|---|---|
| `path` `name` `target` `preset` `kind` | what was measured |
| `wall_s` | end-to-end, minimum across repeats |
| `includes_wall_s` `incl_pct` `own_s` | TU mode only: the stripped half, and the split |
| `frontend_s` `backend_s` `source_s` `instantiate_s` | the `-ftime-trace` breakdown |
| `ours_source_s` `system_source_s` `include_count` | the parse split, and how many files were entered |

`attribution.csv` — one row per (measured file, header it pulled in):
`preset, kind, tu_path, tu_target, header, header_name, header_self_s, is_system`.

This second table is the one worth having, because it answers a question the JSON cannot express:

```
SUM(header_self_s) GROUP BY header      -- total cost of a header across the whole build
COUNT(*)           GROUP BY header      -- how many TUs pay for it
```

A 40 ms header pulled into 200 TUs beats a 300 ms header pulled into three, and only this aggregation says so.
`export` prints that ranking itself, so the first answer needs no spreadsheet at all.

**`header_self_s` is first-include cost within one TU.**
Include guards mean a header included second in the same TU is nearly free, and its cost lands on whichever header pulled it in first.
For ranking aggregate cost that is the right attribution.
For "what would I save by dropping this include from this file" it is not: the saving is zero when something else still pulls the header in.

## Flags

| flag | effect |
|---|---|
| `--repeat N` | compile each file N times, keep the fastest — the low-noise estimator. Use 3+ when comparing small differences. |
| `--target <glob>` | restrict to targets, comma-list and wildcards |
| `--preset a,b` | measure across several presets; each gets its own block |
| `--top K` | rows in the stdout table (default 20) |
| `--out FILE` | where the JSON lands |
| `--no-time-trace` | skip the trace. It costs ~8 % to collect, so drop it when the absolute wall clock matters more than the split. |
| `--keep-tus DIR` | keep the synthetic TUs, to see exactly what was measured |

Measurement is **serial**, always.
A parallel measurement measures the machine, not the code.

## Compiling individual files

```bash
uv run dev.py build --files "libs/base/clean-core/tests/container/*.cc"
```

Compiles just those sources and stops — no link, no rest of the target.
It resolves each source to its object through the compilation database and hands the objects to ninja as targets.
So this still runs through the real build graph: generated headers are produced first, and the objects build **in parallel**.
Twelve clean-core container TUs rebuild in ~2 s this way against ~9.6 s serially.

The go-to loop for a header change that has to compile everywhere before it can be tested anywhere.

## Traps this tooling exists to avoid

Each of these silently produces a plausible wrong number, and each was hit while building it.

- **Flags must be inserted before the `--` separator.**
  CMake ends a compile command with `-c -- <source>`, and everything after `--` is an input file.
  An appended `-ftime-trace` is reported as `no such file or directory`.
- **Objects go to a scratch directory.**
  Reusing the database entry's `/Fo` overwrites the real build's objects.
- **Never `-fsyntax-only`.**
  Headers do real backend work through inline functions, template instantiations and static initializers, so dropping codegen under-reports exactly the headers worth finding.
- **A stripped TU needs its original directory on the include path.**
  A quoted include resolves against the directory of the file containing it, and the synthetic copy does not live there.
- **`-ftime-trace` cannot answer "how much of this TU is its includes".**
  It does not separate work done in a TU's own body from work done in its headers, and the TU is itself a `Source` span.
  Measured end to end, a dx12 test is 84.6 % includes where the trace's `Total Source` suggests 78 %.
  That is why `tu` mode compiles a stripped copy instead of reading it off the trace.
