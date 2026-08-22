# nexus/recording — asking a test what it recorded

Every `nx::run` stands a [`cc::rec`](../../clean-core/docs/systems/recording.md) recorder up for the whole binary, attributes each test's events to that test, and lets a test read its own back.

That gives three things at once, for zero source changes in a test binary:

* a **console logger**, so `CC_LOG_*` from any test or example prints;
* **`nx::test_recording()`**, so a test can assert on what it recorded rather than on a debug getter that exists only to be asserted on;
* **a recording per failing test**, written beside the run's other artifacts.

---

## Reading your own recording

**A test opts in.** Bucketing is per test and paid per test, so a binary of tests that never ask pays nothing.

```cpp
#include <nexus/rec.hh>

TEST("cache warms on first miss", nx::config::recorded)
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

Without `nx::config::recorded`, outside a test, or under `--no-recording`, `is_attached()` is false and everything reports empty.
A test that means to read its recording should say so rather than check for it — `SKIP` is the right answer only for a test that must also survive `--no-recording`.

---

## How a test's events are told apart

Tests run asynchronously and in parallel, so a thread does not identify a test: a logical test runs on whichever workers pick it up, and several are in flight at once.

So nexus mints a **trace id per test** and installs it on `cc::async`'s ambient chain beside the context that already names the test.
Every event recorded under that test — on any worker, across any `co_await` — is then attributed by the `ambient_changed` delta that names the id.
[systems/recording](../../clean-core/docs/systems/recording.md#async-scopes) is that mechanism; nexus is just its first heavy user.

A nested test (one run through `nx::test_registry` from inside another) mints its own id **if it opted in**, and its events then belong to it rather than to its parent.
One that did not mints nothing and stays under whatever context was already in effect, so its events land in the parent's bucket.
That is the honest answer rather than a gap, since the work really did run under the parent.

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

Measured on this repo's own suite, `relwithdebinfo-clang`, median of sixteen runs:

| binary | no recorder | opt-in default | every test bucketed |
|---|---|---|---|
| `clean-core-test` (1194 tests) | ~122 ms | ~155 ms | ~155 ms |
| `nexus-test` (184 tests) | ~540 ms | ~544 ms | ~705 ms |

**Opting in is what makes it free.**
`nexus-test` is the worst case in the repo, since almost every one of its tests runs a whole nested test registry.
Bucketing every test costs it 31%; bucketing the handful that ask costs it nothing measurable.

`clean-core-test`'s ~33 ms is a different cost and does not move with this switch.
It is the ~54 `REC_TEST`s that own the recorder: each hands the singleton over, so each pays a `shutdown` plus an `initialize`, and an `initialize` pre-faults its ready chunks.
Only a binary with recorder-owning tests sees it.

Four switches, and they are different:

* **`nx::config::recorded`** on one test — bucket it, so `nx::test_recording()` can answer and a failure keeps the evidence.
* **`--record`** on the run — bucket **every** test, whatever its own config says.
  A debug flag: it costs the whole suite the right-hand column above, and nothing releases a bucket until its test passes.
  Retention is deliberately unbounded — you asked for the whole recording, so you get the whole recording — which is why this is normally paired with a filter.
* **`nx::config::owns_recorder`** on one test — hand the whole singleton over, for a test that drives `cc::rec::initialize` itself.
* **`--no-recording`** on the run — no recorder at all, so no console logger and no dumps either.
  It wins over `--record`, since there is no recorder left to bucket into.

---

## A failing test's recording

A passing test's events are dropped the moment it ends, which is what returns their chunks to the pool.
A failing test's are kept and written to `test-recording-<name>.ccrec` beside the run's JUnit XML, if one was asked for.

Only for a test that had a bucket — one that opted in, or any test at all under `--record`.
That is what `--record` is for: chasing a failure you did not anticipate, usually as `--record <filter>` once you know which test to watch.

A test that fails without a bucket is not left with nothing, though.
The ambient deltas are in the stream either way, so a crash dump or a whole-run capture still carries the attribution — [systems/recording](../../clean-core/docs/systems/recording.md) has that half.

A test configured `owns_recorder` ends the run's recorder mid-suite, and every failing test's recording is serialized at that moment rather than dropped.
A recording holds chunk references and cannot outlive the pool, so the BYTES are what carry across the handover — without that, one such test anywhere in a binary means no dump is ever written for it.

They are written **at the end of the run**, not when the test fails.
A test finishing does not mean its events are drained, since the actor is a millisecond behind.
Flushing per test would be a process-wide drain thousands of times over, for a file nobody reads until the run is over.

Load one with `cc::rec::load_recording`; [systems/recording-formats](../../clean-core/docs/systems/recording-formats.md) is the format.
CI uploads them inside `ci-logs.zip` — [docs/guides/ci.md](../../../../docs/guides/ci.md) — which is what makes a runner-only failure diagnosable.

---

## A test that owns the recorder

`cc::rec` is a process-wide singleton with one `initialize`/`shutdown` pair, so a test that drives it itself cannot run beside the run's recorder.

`nx::config::owns_recorder` hands it over: the run shuts its recorder down before the body and re-initializes after.
Pair it with `nx::config::exclusive()`, because a recorder torn down on one thread is torn down for every thread.
Expect `nx::test_recording()` to report unattached inside such a test, since there is no run recorder to bucket into.

```cpp
#define REC_TEST(name_) TEST(name_, nx::config::exclusive(), nx::config::owns_recorder)
```
