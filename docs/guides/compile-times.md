# Measuring compile times

`dev.py check --profile` says which *phase* of a run is slow ([Profiling a run](building-and-testing.md#profiling-a-run)).
This is the other half: once you know compilation dominates, `dev.py compile-time` says *which headers and which translation units*.
`dev.py build --files` then lets you iterate on them without building the world.

[docs/notes/build-times.md](../notes/build-times.md) is the findings log these tools feed.

## The two questions

```bash
uv run dev.py compile-time headers "libs/base/clean-core/src/clean-core/container/*.hh"
uv run dev.py compile-time tu      "libs/base/clean-core/tests/container/*.cc"
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
| `trace.top_headers` | the 15 costliest by exclusive self time |

`ours_source_s` versus `system_source_s` is usually the first thing to look at.
On a dx12 test TU it is 0.32 s ours against 0.98 s system (MSVC STL, Windows SDK, `d3d12.h`), which caps what cleaning up our own headers can win.

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
