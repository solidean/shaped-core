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
In a phase with a `main_thread` test the caller runs the bodies handed to it instead.

`-j1` stays first-class, and is not merely "a pool of one".
It drives one test node at a time under a `cc::singlethreaded_scheduler`, in the run's seeded order.
Between drives it pumps the main home and sweeps the thread-pump registry, because a node that hops to main or awaits an unthreaded actor completes only through those.
That makes it the reproducible-debugging mode: a failure at `-jN` that survives `-j1` with the same `--seed` is a test bug, and one that vanishes is a concurrency bug.

## The run seed

**A real run shuffles**: the order tests are handed to their phases, and the order every invocation runs its children in.
The seed comes from the clock unless `--seed N` pins it, and the run prints it first: `nexus: run seed N (reproduce with --seed N)`.
It is printed again beside any failure, and the JUnit report carries it as a `seed` property.
A test that passes only in one order is hiding a dependency, and the printed seed is what makes the failure that exposes it reproducible.

- **A test's seed derives from the run seed and its name**, never its position, so `dev.py test "<one test>" --seed N` hands it the seed it had in the full run.
  `nx::config::seed(n)` pins it; `nx::test_seed()` and `nx::test_random()` read it.
- **A dispatched child's seed derives from its driver's seed, its invocation name and its own name.**
- **An invocation's children are shuffled before `-c` scoping**, so narrowing to one child never changes the order the others ran in.
- **Reports keep schedule and match order** whatever order things ran in.
- **Fuzz tests draw their seed range from the test seed**, so every run explores new programs and a found failure replays under its `--seed`.

A hand-built `test_schedule_config` does not shuffle, for the same reason it defaults to `jobs = 1`: nexus's own meta-tests assert on order.
An example prints no seed, since its transcript is its documentation and it runs one body.

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

| Config item | Where the body runs |
|---|---|
| *(default)* | the run's scheduler, capped by `--jobs` |
| `own_pool(n)` | a private pool of `n` workers, shared with every other test asking for that same count |
| `main_thread` | on the thread `nx::run` was entered on, handed there by its node — a flag on top of the mode, see below |

## The ambient scheduler

A separate axis from the one above: **where the body runs** is one question, **which scheduler the body's own async work belongs to** is another.

Every async needs an ambient scheduler and it is an error to touch one without it, so a run installs one for each phase (`cc::install_compute_async_scheduler`).
A body running as a node on the phase's pool inherits it as a bound worker scope; a directly driven body gets it as the installed compute scheduler.

| Config item | The ambient scheduler |
|---|---|
| *(default)* | a pool — the phase's own where the bodies run on it, otherwise one stood up for the phase |
| `singlethreaded` | a `cc::singlethreaded_scheduler` bound to the body's thread, so every graph runs inline and in order |
| `no_scheduler` | none installed and none bound |

`singlethreaded` is for a test whose subject is the ORDER things run in, which a pool is free to change.

`no_scheduler` is what a test needs when it stands up its own `cc` scheduler, or nests an `nx::execute_tests` run of its own.
Touching an async under it asserts, which is the point: the test has taken that decision over.
`execute_tests` asserts when a scheduler is already bound rather than nesting one, and names the fix.
`nx::invoke_tests` is unaffected — a dispatched child runs inside its driver's body and creates no scheduler.
So a child declaring either mode asserts at dispatch unless its driver declares the same — [invocable-tests](invocable-tests.md#scheduling-asks-belong-to-the-driver) has the rule.

Both drive the body directly, so neither composes with a mode that runs it as a node, and asking for both is an assert.

## Main-thread affinity

Some work must run on the **process main thread** — `sr::window_system` asserts on it, because SDL does.
No `--jobs` value helps: at `-jN` a body runs on whichever worker picks it up, and an exclusion tag excludes tests without choosing a thread.

```cpp
TEST("sr - window system creates and shuts down", main_thread) { … }
```

`main_thread` is a **flag, not a fourth scheduler mode**, so it composes with the modes instead of excluding them.
A test that wants its body on main and also drives async work of its own can say both.

`nx::run` records the thread it was entered on, and `execute_tests` asserts that is the thread it was called on before honouring the flag.
A nested run satisfies that for free: nesting already requires `no_scheduler`, and a directly driven body runs on the outer run's calling thread.
The case that legitimately trips the assert is a run driven from a thread somebody spawned.

**A `main_thread` test is a node in the shared phase like any other, and runs beside it.**
Its node takes the phase's locks wherever it runs, then hands the body to the run thread, which acts as a main loop while the phase runs.
That loop runs handed-over bodies one at a time and pumps the main thread in between — `cc::pump_main_thread`, the call an application's own event loop makes.

The body runs at loop level rather than as a node homed to `cc::main_thread_scheduler()`, and the difference is deliberate.
A home is never re-entered from inside one of its own bodies, so a homed body that blocks on a graph with a main-homed step would wait forever.
At loop level the same wait runs that step, exactly as it would in an application.

So `main_thread` says **which thread**, and nothing else.
It promises no exclusion, not even among main-thread tests: a test that must run alone says `exclusive()`, which `EXAMPLE` bakes in, and one that must not overlap a group says `exclusive(tag)`.
**A body that blocks may run other tests on its stack**, main-thread ones included, because the wait helps drive whatever is queued.
That is fair rather than a defect: a test that cannot tolerate it awaits instead of blocking.
Under `-j1` the run thread drives the nodes one at a time, and a `main_thread` body runs in place.
In a `-jN` phase with no `main_thread` test, the run thread participates in the pool as before.

**On an `ASYNC_TEST` the flag homes the body to main**, with exactly the meaning `cc::make_async_lazy_on_main` has.
Every segment runs on main — the first line, and every resume after a wake from another thread — until the body hops away with a `co_await` of its own.
The run thread's loop is what runs those segments, at `-j1` as at `-jN`.

**`own_pool(n)` is an assert** rather than a quiet demotion, because a private pool's worker is never the main thread.

## Exclusion is locks

```cpp
TEST("sg - clears the backbuffer", exclusive("gpu")) { … }   // never runs beside another "gpu" holder
TEST("env - rewrites the global config", exclusive())        // runs alone, beside nothing at all
```

Each phase holds one `cc::async_shared_mutex` and one `cc::async_mutex` per tag.
A test node takes them before its body: the phase lock shared — or exclusively, for `exclusive()` — and then each of its tags.
It releases them when it resolves, and an `ASYNC_TEST` holds them across every suspend of its graph.

A waiting test parks instead of blocking a worker, so the pool runs other tests while it waits.
That is what lets an `exclusive()` test, an `ASYNC_TEST` and a `main_thread` test all be ordinary nodes in the shared phase, with no barrier and no routing around the graph.

- **Tags are taken in name order, after the phase lock, one at a time**, which is what keeps two multi-tag tests from deadlocking.
- **The phase lock is writer-preferring**: once an `exclusive()` test waits, tests arriving after it wait behind it.
- **The trade: holders run in arrival order, not schedule order.**
  Under `-jN` two holders of a tag no longer run in the order the schedule lists them.
  `-j1` still runs each phase in schedule order, so a failure that depends on the order is still reproducible there.
- **Exclusion across scheduler modes is free**, because phases are sequential; a lock is only ever contended within its phase.

A test may carry up to `nx::config::max_exclusion_tags` tags.
Asking for more is an assert, never a silent drop.

## Why a failing test cannot poison the ones behind it

A test node **always resolves to a value**, never to an error.
A failure is data on the `test_execution`; the async failure channel is not used at all.

That is load-bearing rather than tidiness: the phase's join requires every test node, so an error would propagate into the join and turn one red test into a red phase.

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

**The body must be a coroutine**, and a body handing back anything else fails its test by name.
C++ needs at least one `co_` keyword to make a body a coroutine, so one that awaits nothing ends in a bare `co_return;`.
Nexus places the body before its first line runs, and only a coroutine reserves the home word that placement writes.

**Attribution across the suspension rests on one mechanism.**
Scheduling a **cold** node stamps the scheduling thread's ambient context onto it as a resume token.
Nexus installs this test's link and schedules the body's root under it, so `poll()` re-installs that link on whichever worker picks it up.
The cold nodes that root drives inline inherit it in turn, because a node without a token of its own inherits its driver's.

A coroutine body is cold by construction — [`cc::async`'s coroutines are lazy](../../clean-core/docs/systems/async.md#co_await--co_return) — so the stamp always lands.

Two limits, both deliberate:

* **`SECTION` is not available in an async body**, and asserts.
  The section tree is replay state — the body re-runs once per section path — and an async body runs once.
* **A graph resolving to an error fails the test, naming the error**, and is never propagated onward.
  An awaited dependency that fails is exactly that: it short-circuits the rest of the body, then fails the test.

**Scheduling asks apply as they do to a `TEST`.**
`main_thread` is above; exclusion holds across every suspend; `own_pool(n)` runs the body and what it schedules on that pool.
**An async test awaiting an unthreaded component — an unthreaded actor, cache or io_system — asks for `main_thread`.**
Only a loop that owns its thread drives such a component, and the run's main loop is that loop; a pool thread parked under a plain async test never sweeps it, so the test would wait forever.
clean-core's [Who drives a pump](../../clean-core/docs/systems/async.md#who-drives-a-pump) says why pool threads stay out of it.
`singlethreaded` drives the body to completion in the directly driven phase, inline on the run thread and in order, with the same main-home and pump servicing `-j1` has.
`no_scheduler` is refused: nothing would drive the body.

**`SKIP` and `REQUIRE` behave as in a `TEST`**, at any depth below the body.
Their throw ends the poll it happens in, and cc::async turns it into that node's error; nexus marks the test before throwing, so that error is the abort rather than a second failure.

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
