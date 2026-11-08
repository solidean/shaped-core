---
created: 2026-11-08
priority: someday
refs:
  - file: libs/base/clean-core/src/clean-core/thread/async.cc
  - file: libs/base/clean-core/docs/systems/async.md
  - file: libs/base/clean-core/src/clean-core/thread/async_parallel_for.hh
  - test: "async backlog - a parked frame dropped while work below it runs is destroyed at once, so that work must own its state"
---

# Work that outlives its frame: a dropped parked frame dies under the work it started

A parked frame dropped unresolved is destroyed at once, while the work below it runs on, abandoned.
Work that reaches back into that frame, through `[&]` captures or a pointer to its locals, then writes freed memory.
Today the rule is the call site's: work a frame starts owns what it touches ([Work that outlives its frame](../systems/async.md#work-that-outlives-its-frame)).
That rule is easy to break and hard to see broken, so this asks, with a fresh look, whether the runtime or the library should carry it instead.

## Why it keeps coming back

- **The broken shape is the natural one.** A parallel helper as a lambda coroutine, `[&]` captures, children capturing `&f`, per-task outputs in a vector the children index.
- **It is correct until someone drops it.** Awaited to completion it is fine; a cancellation, a cache eviction or an unread prewarm turns it into a use-after-free.
- **The symptom is far from the cause.** A crash or a double free inside a child, or in a test a "left async work running" report and nothing else.
- **Our own guidance points at it.** `cc::async_parallel_for` owns its body but says to capture by reference what the body works on, which lives in the awaiting frame.
- **Measured once, 2026-11-08:** a consumer's ownership-style export, dropped mid-flight by an evicted cache entry, double-freed a per-task vector under ASan.
  The same abandoned children carried a test's context past its end, the leftover-work flake that had been chased for a day.

## Constraints any answer must keep

- **The hot path stays as it is.** Create, drive, resolve and drop are the most frequent operations in the system; a fix there must measure as nothing, in instructions retired.
- **Dropping stays cheap and safe.** Cancellation and prewarms are legitimate, and a drop must not block, wait or leak.
- **A pool may die with work queued.** Abandoning a graph is allowed, so whatever waits on work must not wait on work that will never run.
- **The graph is a DAG, not a tree.** A node may be awaited by several frames, and no frame owns its deps exclusively.
- **Homes keep their order.** A homed frame's teardown runs where its home says.

## The options, assessed

### 1. The runtime keeps a dropped frame until the work below it settles

Tried on 2026-11-08 and taken back.
The never-resolved teardown walked the parked node's deps, through parked deps under their locks, and deferred the frame's destruction behind a latch on each running node.

- It fixed the crash, and with care it cost the happy path nothing measurable: it lived on the cold teardown arm, in its own translation unit.
- **It waits on work that may never run:** a node queued in a pool that is then destroyed never resolves, so its latch never fires and the frame leaks.
  Releasing such nodes from each scheduler's drop points covered most of it, not all: a node running when latched may later park on work the dead pool dropped.
- **It needs a re-check machinery** for that case, a registry of pending deferrals with cancellable latches, a third layer for what was meant to be narrow.
- **It covers only work reachable through deps.** A child a frame started but never awaited reaches into it just the same and is not below it.
- **It pins memory on behalf of abandoned work**, for as long as that work runs, which is the opposite of what a cancellation wants.
- Verdict: the reach is wrong, since "below" in the graph is not "may touch this frame", and its edge cases grow with each scheduler.

### 2. Call-site discipline: the work owns its state (today)

- Correct and free for the runtime; the shapes are a shared state, returned results, or copies only.
- It relies on everyone knowing, and the broken shape compiles, passes tests and works until the first drop.
- Verdict: the floor, never the whole answer; what follows makes it the easy path and makes breaking it visible.

### 3. A fan-out primitive whose children cannot reach the frame

- A `cc::async_parallel_for` sibling, or a revision of it, that owns everything the children use.
  The body and its captures move into the operation's own shared state, and outputs are returned per chunk rather than written into the caller.
- The caller writes the natural loop, and the primitive makes it safe, for the common case of a fan-out over an index range with per-task results.
- It does not cover free-form children a frame starts itself.
- Open: whether the existing `async_parallel_for` can change its advice and its examples without its callers paying a copy, since capturing big inputs by reference is the point there.
  A middle ground: reads by reference stay allowed for inputs that outlive the operation by construction, outputs go through the primitive.

### 4. Opt-in scoped children that keep their creator's frame alive

- `make_async_lazy_scoped`, or a flag, making a child hold a strong reference to the node whose frame created it, dropped when the child resolves or is destroyed.
- Exact: it covers awaited and unawaited children alike, without walking anything, and costs only the code that asks for it.
- The cycle it makes is the risk: the frame holds the child, the child holds the frame, and a child that never runs, queued in a dead pool, keeps both alive.
  Breaking it needs dead schedulers to resolve their unrun nodes as cancelled, which releases their captures, a defined meaning for "dropped, not run".
- Open: what the extra reference costs where the flag is set, where in the node it lives without growing every node, and whether cancel-on-pool-death is wanted on its own merits.
- **Its manual form exists since 2026-11-08:** `co_await cc::async_this_node()` gives a coroutine a strong handle to its own node, which its children capture.
  A consumer's long export coroutine uses it, since moving its many phases' locals into shared state was the larger change; the dead-pool leak is accepted there.
  The flag would be that, automatic, and with the cycle broken.

### 5. Cooperative cancellation that joins

- A drop cancels the work below and waits for running work to observe it before the frame goes; structured concurrency in the strict sense.
- It gives the strongest guarantee, and the highest cost: every child must check for cancellation, a drop can block, and shared deps make "the work below" ambiguous.
- Verdict: wrong default for a cooperative, shared graph; maybe right inside a primitive like 3, where the children are its own.

### 6. Make the hazard loud

- **A lint:** shaped-linter flags a lazy or scheduled child whose lambda captures by reference inside a coroutine body, the shape that breaks; cheap, catches recurrence at review time.
- **A debug diagnostic:** in debug builds, a parked frame torn down while it abandons running work is logged once with the frame's and the work's creation sites.
  The event is rare and cold, so the check costs nothing on the hot path, and it turns "a crash far away" into "this drop, here".
- **ASan in the gate** for the async-heavy consumers, since it found this at once where days of reruns did not.
- Verdict: worth doing whatever else is chosen, since it is what makes 2 hold.

## Recommendation, as of writing

- **Now:** keep 2, with the docs and with `async_this_node` for frames that cannot give their state away, and add 6: the lint and the debug diagnostic first, both cheap.
- **Next:** 3, a fan-out primitive that owns its state, since the parallel-helper shape is where nearly every occurrence so far came from.
- **Evaluate:** 4, opt-in scoped children, together with cancel-on-pool-death, measured on the node layout before anything else.
- **Rejected:** 1 as a general mechanism, for the reasons above; and 5 as a default.

## What a revisit should start from

- The consumer-side fixes made 2026-11-08 are the corpus: each shows the shape that broke and the shape that replaced it.
- The test named above pins today's contract, and must be rewritten if the contract changes.
- The deferred-teardown attempt is in the history of this issue's day: its walk and latch code are a starting point for 4's cancel-on-pool-death, not for 1.
