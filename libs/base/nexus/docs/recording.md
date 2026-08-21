# nexus/recording — asking a test what it recorded

Every `nx::run` stands a [`cc::rec`](../../clean-core/docs/systems/recording.md) recorder up for the whole binary, attributes each test's events to that test, and lets a test read its own back.

That gives three things at once, for zero source changes in a test binary:

* a **console logger**, so `CC_LOG_*` from any test or example prints;
* **`nx::test_recording()`**, so a test can assert on what it recorded rather than on a debug getter that exists only to be asserted on;
* **a recording per failing test**, written beside the run's other artifacts.

---

## Reading your own recording

```cpp
#include <nexus/rec.hh>

TEST("cache warms on first miss")
{
    auto rec = nx::test_recording();

    load_asset("mesh.obj");
    rec.sync();
    CHECK(rec.all().count("cache-miss") == 1);

    load_asset("mesh.obj");
    auto const since = rec.sync();          // only what arrived in between
    CHECK(since.count("cache-miss") == 0);  // ... the second load hit
    CHECK(rec.all().count("cache-miss") == 1);
}
```

`all()` accumulates across every sync and answers *did this ever happen*.
`sync()` returns the delta and answers *did this happen in the window I just opened*.
Both hand back a `cc::rec::recording`, so the whole [query API](../../clean-core/docs/systems/recording.md#the-algebra-and-asking-questions-of-a-recording) applies.
That is `count`, `contains_in_order`, `first_value`, `messages` and `scopes`.

**`sync()` is the only call here that touches the recorder**, and it drains the actor under a process-wide mutex.
A few per test costs nothing; one per check inside a loop is a different thing entirely, which is why this is a handle you hold rather than a comparison you write inline.

Outside a test, or in one declared `nx::config::no_recording`, `is_attached()` is false and everything reports empty.

---

## How a test's events are told apart

Tests run asynchronously and in parallel, so a thread does not identify a test: a logical test runs on whichever workers pick it up, and several are in flight at once.

So nexus mints a **trace id per test** and installs it on `cc::async`'s ambient chain beside the context that already names the test.
Every event recorded under that test — on any worker, across any `co_await` — is then attributed by the `ambient_changed` delta that names the id.
[systems/recording](../../clean-core/docs/systems/recording.md#async-scopes) is that mechanism; nexus is just its first heavy user.

A nested test (one run through `nx::test_registry` from inside another) mints its own id, so its events belong to it and not to its parent.
Attribution nests logically; buckets do not.

---

## One listener, not one per test

The run installs **a single** bucketing listener, which slices each block into ambient segments and files each under the trace it belonged to.

A listener per in-flight test would be the obvious design and the wrong one.
Every listener callback runs under `cc::rec`'s single processing mutex, so N concurrent tests would mean N full scans of every block, serialized.
That puts the cost on the tests that record the *most* rather than on the ones asking questions.

The single listener scans once.
Attribution only changes where a delta says it does, so between two deltas it extends one byte range rather than looking at events at all.
A block therefore yields one slice per **segment** rather than one per event.
A segment whose trace has no bucket costs one lookup and is dropped.

---

## What it costs

Measured on this repo's own suite, `relwithdebinfo-clang`, median of a dozen runs:

| binary | `--no-recording` | default | delta |
|---|---|---|---|
| `clean-core-test` (1194 tests) | ~121 ms | ~155 ms | **+28%** |
| `nexus-test` (184 tests, most running nested registries) | ~536 ms | ~705 ms | **+32%** |

The cost is per **test**, not per event: a trace link, two scope events and the ambient deltas that carry them.
A binary of a few long tests barely notices; `nexus-test` is the worst case in the repo because almost every one of its tests runs a whole nested test registry.

Two switches, and they are different:

* **`nx::config::no_recording`** on one test — no trace, no bucket, and `nx::test_recording()` reports unattached.
  For a test that records enough not to be worth keeping.
* **`--no-recording`** on the run — no recorder at all, so no console logger and no dumps either.
  For timing the tests themselves.

---

## A failing test's recording

A passing test's events are dropped the moment it ends, which is what returns their chunks to the pool.
A failing test's are kept and written to `test-recording-<name>.ccrec` beside the run's JUnit XML, if one was asked for.

They are written **at the end of the run**, not when the test fails.
A test finishing does not mean its events are drained, since the actor is a millisecond behind.
Flushing per test would be a process-wide drain thousands of times over, for a file nobody reads until the run is over.

Load one with `cc::rec::load_recording`; [systems/recording-formats](../../clean-core/docs/systems/recording-formats.md) is the format.

---

## A test that owns the recorder

`cc::rec` is a process-wide singleton with one `initialize`/`shutdown` pair, so a test that drives it itself cannot run beside the run's recorder.

`nx::config::owns_recorder` hands it over: the run shuts its recorder down before the body and re-initializes after.
Pair it with `nx::config::exclusive()`, because a recorder torn down on one thread is torn down for every thread.
Expect `nx::test_recording()` to report unattached inside such a test, since there is no run recorder to bucket into.

```cpp
#define REC_TEST(name_) TEST(name_, nx::config::exclusive(), nx::config::owns_recorder)
```
