# profiling — scopes, stats, and choosing between them

Profiling in shaped-core is one vocabulary over the recording stream, the same one logging writes into.
The bet the whole design rests on is that annotation which is cheap is annotation that stays in the code, so nothing here is meant to be compiled out for a release.

The mechanism is [systems/recording](systems/recording.md); this is what to reach for and where to put it.

```cpp
#include <clean-core/common/profiling.hh>   // scopes, values, markers, stats
```

## Which tool

| you want to know | reach for |
|---|---|
| how long a block of one thread's work took | `CC_RECORD_SCOPE` |
| how long a logical operation took, across threads and suspends | `CC_RECORD_ASYNC_SCOPE` |
| where time actually goes in code nobody annotated | the sampler |
| the current reading of a quantity | `CC_RECORD_STAT` |
| how much of something happened | `CC_RECORD_ACCUM` |
| whether a branch was ever taken | `CC_RECORD_MARK` |

The first rule is that **you do not have to guess where the time goes**.
The sampler finds slow code on its own, including code nobody thought to annotate — so a scope is for naming work a person wants named, not for hunting.

## Scopes

```cpp
CC_RECORD_SCOPE();               // named after the enclosing function
CC_RECORD_SCOPE("upload-pass");  // ... or explicitly
```

A scope is strictly thread-local and strictly non-suspending: it opens and closes on the same thread, at the same nesting depth.
**A scope that crosses a `co_await` is a lie** — the work stopped and the thread went elsewhere — and `cc::async`'s frame driver asserts on exactly that rather than reporting a wrong number.

For a span whose two ends are in different functions there is the unmatched form:

```cpp
CC_RECORD_SCOPE_BEGIN("sv.frame");   // ... and the caller owes an END, on the same thread
CC_RECORD_SCOPE_END("sv.frame");
```

An unbalanced pair produces a wrong trace rather than a diagnostic, so prefer the block form wherever the span fits a block.

### Conditional scopes

```cpp
CC_RECORD_SCOPE_IF(bytes.size() >= scope_threshold_bytes, "json.read");
```

**The condition is read once, at entry**, and does not affect whether the scope closes.
A skipped scope consumes no depth either, so nothing nested under it is re-nested a step too deep.

This is for sites where the **event rate** is the problem, not the duration.
A JSON parse of a twelve-byte value called a million times a frame would bury the stream; the same call on a forty megabyte document is exactly the span you want.
Give the threshold a name rather than writing a number at the site — `babel::json`'s is `scope_threshold_bytes`.

### Async scopes

An async scope rides `cc::async`'s ambient chain, so it follows the work to whichever worker picks it up, across suspends and across threads.
It nests logically rather than as a stack frame, and popping it does not require the work started under it to have finished.

```cpp
CC_RECORD_ASYNC_SCOPE("bcache.acquire");
```

**Open one around the code that BUILDS the async work, never inside the coroutine body.**
A scope object living in a coroutine frame is entered and left at suspension points rather than around them, and that fast-fails rather than diagnosing.
`blob_cache::acquire` is the worked example: the scope sits in the plain function that constructs the pipeline, and the coroutine inherits it through the chain.

It costs two refcounted allocations against a scope's two thread-local writes, so it is the right trade for a request, a frame or a job, and the wrong one for an inner loop.

## Stats

Two kinds, and picking the wrong one produces a plausible graph of the wrong thing.

```cpp
CC_RECORD_STAT("sg.command_lists.live", cc::rec::unit_count, n);      // a SNAPSHOT
CC_RECORD_ACCUM("sg.upload.bytes", cc::rec::unit_bytes, size);        // a DELTA to add up
```

A **snapshot** is the current reading of something that exists whether or not you look: queue depth, resident bytes, frame time.
Summing snapshots is meaningless; averaging them is not.

An **accumulate** is a delta to add up: bytes uploaded, cache hits, tasks finished.
Summing them is the whole point.

Values are `f64` only, which also covers every integer up to 2^53 — one numeric type means a listener can graph anything without a type switch.

The unit is a `cc::rec::unit`, a plain struct rather than an enum, so adding one breaks nobody.
`unit_count`, `unit_bytes`, `unit_seconds`, `unit_ratio` and `unit_hertz` come with `cc::rec`; define your own next to the code that records it.

### Mirror a counter, do not duplicate it

Where a library already keeps a counter, record it **where that counter is written** rather than at the call sites.
`blob-cache` does this in the one adder every stat bump goes through, which is what stops the recorded numbers and the in-process `cache_stats` struct from ever disagreeing.

## Where to put a scope

The useful question is what a reader would want to see in a flame graph, not what is slow.

- **Entry points**, at the granularity a caller thinks in — "load an image", not "decode a PNG chunk".
- **Startup and shutdown**, where hangs live.
  The thread pool's constructor and destructor carry one for exactly this reason.
- **Stalls**, where one side waits for another — `sg.epoch.wait` is the CPU waiting on the GPU, and its duration is the answer to "am I GPU-bound".
- **Anything that compiles, loads or allocates from the OS**, which is where the milliseconds are.

And where not to:

- **Inner loops**, per-element and per-property work.
  `vdoc`'s one-entity edit is roughly ten microseconds: it carries a handful of events comfortably and one per property not at all.
- **Anything the recorder itself calls.** `capture_stack` is deliberately uninstrumented, because the recorder uses it on its own error path.
- **Per-row or per-item paths**, which want an accumulate instead.
  `babel::sqlite` counts stepped rows rather than scoping each one.

## Sampling

Scopes say what a person named; the sampler says what actually ran.

```cpp
#include <clean-core/record/sampling.hh>
cc::rec::sampling_scope const s({.rate_hz = 1000.0});
```

A sample stops at the innermost open scope, so a sample inside instrumented code is often one address — the scope stack already names everything below it.
That is the point, and it is why scopes and sampling are worth having together rather than as alternatives.

## Looking at it

`babel::chrome_trace` writes a recording as Chrome Trace Event JSON, for `chrome://tracing` and `ui.perfetto.dev`:

```bash
uv run dev.py example babel-serializer/chrome-trace
```

Domains become categories there, so a viewer can show one subsystem at a time.

## Turning it off

Everything above gates on its domain, per category, as one relaxed store:

```cpp
cc::rec::find_domain("babel.json")->set_enabled(cc::rec::category::profiling, false);
cc::rec::set_all_domains_enabled_mask(mask);   // applies to domains registered later too
```

A disabled site costs one load and a test.
If that ever stops being true enough, `cc::rec::measure_overhead()` measures it on the machine that matters rather than on someone else's.
