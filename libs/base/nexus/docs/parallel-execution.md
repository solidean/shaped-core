# Running tests in parallel

A run is a **graph of `cc::async` nodes**, one per test, driven by a scheduler.
Nexus owns no thread pool, no job queue and no admission control; which thread picks up which test is the scheduler's business.

`TEST` is not "the synchronous case" — it is a test node whose body happens never to suspend.
There is one node kind, and parallelism falls out of that rather than being bolted beside it.

The check-attribution contract this rests on is [threaded-checks](threaded-checks.md); the async system underneath is [cc::async](../../clean-core/docs/systems/async.md).

## `--jobs N`

`--jobs N`, `-j N` or `-jN` caps how many tests may run at once; `-j0` means the machine's hardware concurrency.

**The default is `-j0`** — every core, because a test suite that only runs correctly one at a time is hiding something.
`-jN` for N > 1 builds a join over the whole phase and drives it on a `cc::async_thread_pool` of N-1 workers, the caller participating as the Nth.

`-j1` stays first-class, and is not merely "a pool of one".
It drives one test node at a time under a `cc::singlethreaded_scheduler`, so the run order **is** the schedule order.
That makes it the reproducible-debugging mode: a failure at `-jN` that survives `-j1` is a test bug, and one that vanishes is a concurrency bug.

A **hand-built** `test_schedule_config` defaults the other way, to `jobs = 1`.
Only `create_from_args` starts at 0, so the parallel default belongs to a real run.
A test that builds its own schedule is usually asserting something about the order it runs in, and nexus' own meta-tests are all of that kind.

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

## Main-thread affinity

Some work must run on the **process main thread** — `sr::window_system` asserts on it, because SDL does.
No `--jobs` value helps: at `-jN` a body runs on whichever worker picks it up, and an exclusion tag orders tests without choosing a thread.

```cpp
TEST("sr - window system creates and shuts down", main_thread) { … }
```

`main_thread` is a **flag, not a fourth scheduler mode**, so it composes with the modes instead of excluding them.
A test that wants its body on main and also drives async work of its own can say both.

`nx::run` records the thread it was entered on, and `execute_tests` asserts that is the thread it was called on before honouring the flag.
A nested run satisfies that for free: nesting already requires `no_scheduler`, and the no-scheduler group runs bodies on the outer run's calling thread.
The case that legitimately trips the assert is a run driven from a thread somebody spawned.

The flag is honoured today by driving the body in the no-scheduler group, which already runs bodies directly on the calling thread.
That is the implementation and not the contract: `main_thread` promises a thread, `no_scheduler` promises an absent scheduler, and they are asked for separately.
So main-thread tests share that one phase and run in schedule order among its other members, and cannot yet overlap the shared phase — a quality-of-implementation gap, not a property of the API.

Two combinations are asserts rather than quiet demotions:

* **`own_pool(n)`**, because a private pool's worker is never the main thread.
* **`ASYNC_TEST`**, because the graph it returns is driven by the phase's scheduler and not by the thread the body started on.

## Exclusion is an ordering edge

```cpp
TEST("sg - clears the backbuffer", exclusive("gpu")) { … }   // never runs beside another "gpu" holder
TEST("env - rewrites the global config", exclusive())        // runs alone, beside nothing at all
```

A test **requires the last holder of each tag it carries**, and becomes that tag's new tail.
No-arg `exclusive()` runs alone, beside nothing at all.

**A no-arg `exclusive()` is not scheduled as a node.**
A test that runs beside nothing gains nothing from a graph, and as a barrier would cost an edge to every test before it plus a stalled pool.
So a synchronous one is routed into the **no-scheduler group** instead, which already runs bodies one at a time on the calling thread — the same guarantee, for free.

Two consequences worth knowing:

* Such a test **does not order against the shared phase** any more, only within the no-scheduler one.
  It still runs alone, and the order is still reproducible; it is simply no longer a seam that the tests around it sit before and after.
* An **`ASYNC_TEST` keeps its scheduler**, because nothing would drive the root it returns.
  So does a test that asked for `own_pool`.

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

An `ASYNC_TEST` is a test whose body may `co_await`.

```cpp
#include <nexus/async-test.hh>   // a separate header: TEST pays nothing for the async templates

ASYNC_TEST("cache - resolves a miss")
{
    auto const entry = co_await cache.acquire_async("shader.hlsl");
    CHECK(entry.is_compiled());
}
```

The body *is* the graph: nexus schedules it and makes the test wait on it, so a park inside parks the test instead of blocking a worker.

C++ needs at least one `co_` keyword to make a body a coroutine.
A body that awaits nothing therefore ends in a bare `co_return;`, or stays a plain body that **returns** the graph to await — the pre-coroutine spelling, still supported:

```cpp
ASYNC_TEST("...") { return cc::make_async_lazy<cc::unit>(/* ... */); }
```

**Attribution across the suspension rests on one mechanism.**
Scheduling a **cold** node stamps the scheduling thread's ambient context onto it as a resume token.
Nexus installs this test's link and schedules the body's root under it, so `poll()` re-installs that link on whichever worker picks it up.
The cold nodes that root drives inline inherit it in turn, because a node without a token of its own inherits its driver's.

That is why **the root must be cold**: one already scheduled or already resolved cannot take the stamp, and its checks would be billed to whatever happened to be driving.
An already-scheduled root is an assert, not a silent misattribution.
A coroutine body is cold by construction — [`cc::async`'s coroutines are lazy](../../clean-core/docs/systems/async.md#co_await--co_return) — so the rule binds only the returning form.

Two limits, both deliberate:

* **`SECTION` is not available in an async body**, and asserts.
  The section tree is replay state — the body re-runs once per section path — and an async body runs once.
* **A graph resolving to an error fails the test, naming the error**, and is never propagated onward.
  An awaited dependency that fails is exactly that: it short-circuits the rest of the body, then fails the test.

`no_scheduler` and `ASYNC_TEST` are mutually exclusive: nothing would drive the graph.

## A failing `CC_ASSERT` reports as a check, from any thread

Nexus installs its assert-to-check handler twice, because clean-core's handler stack is per-thread.
Once around every body, on the thread running it, and once as the **process-wide fallback** for the whole run.
The fallback is what covers pool workers driving an `ASYNC_TEST`'s graph, and threads a test started itself.
Attribution is the ambient context's either way, so the check lands on the right test rather than on whatever was running.

This is also why a `CHECK_ASSERTS` block is safe at `-jN`: the throwing handler it installs is visible only on its own thread.

## A failure names what ran beside it

Under `-jN` a failure is usually about what it ran *beside*, and naming only the test that failed leaves you guessing which pair collided.

So both reports answer that, off the same per-thread table:

* The **crash-context hook** lists all tests in flight, one fixed slot per thread, allocation-free — the faulting thread is often not the interesting one.
* A **failing check** gains an `also running: "…"` annotation, listing the other threads' tests.
  Nothing is added at `-j1`, where there is no other thread.

The slot holds the test *declaration* rather than a name pointer and length, because the check reader runs while other threads are still writing to the table.
One word cannot tear, and a declaration outlives the run, so a racing reader sees the previous test or the next one — never a pointer paired with the wrong length.

What it reports is a snapshot, not a fact: a slot may change while the table is walked, so a name means "was running around now".
That is the right resolution for the question it answers, and no lock could sharpen it without changing what is being measured.

## Not here yet: a bare-pool mode

Phases being sequential means a run with several scheduler modes cannot overlap them.
If that ever costs real throughput, the answer is a **fourth scheduler mode**: a *bare pool*, whose group runs on ordinary threads rather than as async nodes.

Its tests would want the async machinery driven at about `-j2` internally rather than `-j1`.
Not for speed — `-j1` there would hide exactly the async-related races this whole design exists to surface.

Nobody has measured a case that needs it.
Do not build it speculatively.
