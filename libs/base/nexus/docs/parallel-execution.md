# Running tests in parallel

A run is a **graph of `cc::async` nodes**, one per test, driven by a scheduler.
Nexus owns no thread pool, no job queue and no admission control; which thread picks up which test is the scheduler's business.

`TEST` is not "the synchronous case" — it is a test node whose body happens never to suspend.
There is one node kind, and parallelism falls out of that rather than being bolted beside it.

The check-attribution contract this rests on is [threaded-checks](threaded-checks.md); the async system underneath is [cc::async](../../clean-core/docs/systems/async.md).

## `--jobs N`

`--jobs N`, `-j N` or `-jN` caps how many tests may run at once; `-j0` means the machine's hardware concurrency.

**The default is `-j1`**, and `-j1` is not merely "a pool of one".
It drives one test node at a time under a `cc::singlethreaded_scheduler`, so the run order **is** the schedule order — the property every existing test was written against.
`-jN` for N > 1 builds a join over the whole phase and drives it on a `cc::async_thread_pool` of N-1 workers, the caller participating as the Nth.

Report order never depends on either: results are written into pre-sized slots by index, and the `--verbose` trace is buffered per test and flushed in schedule order.

## A test body runs with no scheduler bound

The run's scheduler drives **tests**, never the work inside one.

Nexus unbinds it (`cc::async_no_worker_scope`) for the duration of every body, so a test sees exactly the thread it always saw: nothing above it, and its own graphs driven by its own scheduler.
Left bound, a node the *body* scheduled would land in the *run's* queue, to be run later — after the test returned, outside the lifetime of everything its frame captured.

The consequence worth knowing: work a test hands to `cc::async` and does not await is still leaked work, and still fails that test by name.
Nexus will not quietly finish it for you.

## Scheduler modes

Set per test, orthogonal to buckets and to exclusion.
Tests sharing a mode form one graph and run as one **phase**; phases run one after another, because schedulers do not nest.

| Config item | What the test gets |
|---|---|
| *(default)* | the run's scheduler, capped by `--jobs` |
| `own_pool(n)` | a private pool of `n` workers, shared with every other test asking for that same count |
| `no_scheduler` | no scheduler bound at all; bodies driven directly on the calling thread, in schedule order |

`no_scheduler` is what a test needs when it stands up its own `cc` scheduler, or nests an `nx::execute_tests` run of its own.
`execute_tests` asserts when a scheduler is already bound rather than nesting one, and names the fix.
`nx::invoke_tests` is unaffected — a dispatched child runs inside its driver's body and creates no scheduler.

## Exclusion is an ordering edge

```cpp
TEST("sg - clears the backbuffer", exclusive("gpu")) { … }   // never runs beside another "gpu" holder
TEST("env - rewrites the global config", exclusive())        // runs alone, beside nothing at all
```

A test **requires the last holder of each tag it carries**, and becomes that tag's new tail.
No-arg `exclusive()` is a barrier: it follows everything before it in its phase, and everything after follows it.

Every edge points backwards in schedule order, so the result is a DAG by construction.
That is why there is no admission control, no deadlock to reason about, and no starvation to guard against.

Two properties fall out, and both are features rather than accidents:

* **Exclusion fixes the order, not merely the exclusion.**
  Holders of a tag run in schedule order within a phase, and in phase order across phases — reproducible, not "whichever won the lock".
* **Exclusion across scheduler modes is free**, because phases are sequential; only within-phase pairs need an edge.

A test may carry up to `nx::config::max_exclusion_tags` tags.
Asking for more is an assert, never a silent drop.

## Why a failing test cannot poison the ones behind it

A test node **always resolves to a value**, never to an error.
A failure is data on the `test_execution`; the async failure channel is not used at all.

That is load-bearing rather than tidiness: an exclusivity edge feeds one test node into the next, so an error would propagate into every test ordered behind it and turn one red test into a red phase.

## `ASYNC_TEST`

```cpp
#include <nexus/async-test.hh>   // a separate header: TEST pays nothing for the async templates

ASYNC_TEST("cache - resolves a miss")
{
    auto entry = cache.acquire_async("shader.hlsl");
    return cc::make_async_lazy<cc::unit>(
        [entry](cc::async_context<cc::unit>& actx) -> cc::async_step_status
        {
            if (!actx.require(entry))
                return actx.wait_for_dependencies();
            CHECK(entry->has_value());
            return actx.resolve_to_value(cc::unit{});
        });
}
```

The body runs to its `return` exactly like a `TEST` body: same thread, no scheduler bound, its own graphs its own business.
What it *returns* is different — nexus schedules that root and makes the test wait on it, so a park inside the graph parks the test instead of blocking a worker.

**Attribution across the suspension rests on one mechanism.**
Scheduling a **cold** node stamps the scheduling thread's ambient context onto it as a resume token.
The wrapper installs this test's link and schedules the returned root under it, so `poll()` re-installs that link on whichever worker picks the root up.
The cold nodes the root drives inline inherit it in turn, because a node without a token of its own inherits its driver's.

That is why **the returned root must be cold**: a root that was already scheduled or already resolved cannot take the stamp, and the graph's checks would be billed to whatever happened to be driving.
An already-scheduled root is an assert, not a silent misattribution.

Two limits, both deliberate:

* **`SECTION` is not available in an async body**, and asserts.
  The section tree is replay state — the body re-runs once per section path — and an async body runs once.
* **A graph resolving to an error fails the test, naming the error**, and is never propagated onward.

`no_scheduler` and `ASYNC_TEST` are mutually exclusive: nothing would drive the graph.

## Crash reports name every running test

The crash-context hook reports **all** tests in flight, one fixed slot per thread, allocation-free.
Under `-jN` the faulting thread is often not the interesting one.

## Not here yet: a bare-pool mode

Phases being sequential means a run with several scheduler modes cannot overlap them.
If that ever costs real throughput, the answer is a **fourth scheduler mode**: a *bare pool*, whose group runs on ordinary threads rather than as async nodes.

Its tests would want the async machinery driven at about `-j2` internally rather than `-j1`.
Not for speed — `-j1` there would hide exactly the async-related races this whole design exists to surface.

Nobody has measured a case that needs it.
Do not build it speculatively.
