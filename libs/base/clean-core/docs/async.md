# cc::async — a value/dataflow async system

`cc::async<T>` is a low-overhead async for **compute-heavy dependency graphs**. The mental model is
values and dataflow transformations, not futures/promises or callback chains. It is the foundation for the
CPU fan-out "task system" that `cc::threaded_actor` defers to.

Headers: [`clean-core/thread/async.hh`](../src/clean-core/thread/async.hh) (public, templated) and
[`clean-core/thread/async_node.hh`](../src/clean-core/thread/async_node.hh) (untemplated core + scheduler
seam). This is **incubator-stage** API — expect it to grow and change.

## Model

An `async<T>` is an eventual `result<T, async_error>` produced by a **compute frame**: a callable /
state-machine polled through an `async_context`. Failure and cancellation are **values** (`async_error`), not
exceptions or out-of-band control flow.

```cpp
auto a = cc::make_async_scheduled<int>([](cc::async_context&) { return 40; });
auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);   // b depends on a; f gets a plain int
int v = cc::async_blocking_get(b);   // drives the graph on this thread -> 42
```

Two handle flavors:

* **`shared_async<T>` = `std::shared_ptr<async<T>>`** — the normal, composable handle. Many dependents may
  observe it. `async<T>` itself is non-copyable and immovable; you copy the `shared_ptr`, never the node.
  (clean-core has no shared pointer yet, so `std::shared_ptr` is used deliberately, as in `pinned_data`.)
* **`once_async<T>`** — single-consumer, single-continuation. Used internally for owned fork/join children
  (`async_context::await` / `spawn_child`). A second subscription is a hard assert. Not a default public type
  — it is a footgun if used as one.

### The raw compute frame

A frame is a callable `async_result<T>(async_context&)`. It may be a hand-written state machine that adds
dependencies dynamically as it runs:

```cpp
auto a = cc::make_async_lazy<int>(
    [step = 0, child = static_cast<cc::once_async<int>*>(nullptr)](cc::async_context& actx) mutable
        -> cc::async_result<int>
    {
        switch (step++)
        {
        case 0:
            child = actx.spawn_child([](cc::async_context&) -> cc::async_result<int> { return 10; });
            return actx.wait_for_dependencies();
        default:
            return actx.success(*child->value_ptr() + 5);
        }
    });
```

A frame that has more than one kind of `return` must annotate `-> cc::async_result<T>` (the tag helpers all
have different types, so `auto` deduction won't unify them).

`async_context` gives a frame:

* `require(dep) -> bool` — true if `dep` is already ready (read its value now); otherwise records it as a
  pending dependency and returns false. **No subscription happens here.**
* `spawn_child(f) -> once_async<CT>*` — create an owned child from a frame, register it as a dependency, and
  return a handle to read on resume. `await(f)` is `spawn_child` + `wait_for_dependencies`. The child frame
  may omit its `async_context&` too.
* result helpers: `success(v)`, `error(async_error | any_error)`, `wait_for_dependencies()`, `yield()`.

A frame is **re-entrant**: one that waits is re-polled once its dependencies are ready. A typical two-phase
frame (register deps → `wait`, then compute) therefore runs twice. It is never entered again after it
produces a value or error — the frame is destroyed the moment it completes.

### Composition without hand-writing a frame

Most compositions don't need a raw frame. `make_async_lazy` / `make_async_scheduled` are **variadic in their
dependencies**: extra arguments are `shared_async`s (or a `std::shared_ptr<once_async>`) that are awaited and
**unwrapped** to plain values before `f` runs, with errors short-circuiting (the first failed dependency
propagates and `f` is skipped). `f` may take a leading `async_context&` or omit it entirely — the wrapper
adapts either way, and a no-dependency `f` may drop the context too.

```cpp
auto a = cc::make_async_scheduled<int>(/* ... */);
auto b = cc::make_async_scheduled<int>(/* ... */);
auto c = cc::make_async_lazy([](int x, int y) { return x + y; }, a, b);   // c waits for a,b; f gets plain ints
auto d = cc::make_async_lazy([] { return 7; });                          // no deps, no context

// a once_async is consumed (owned) by the single make_async_* / make_once_* call it is passed to:
auto once = cc::make_once_lazy([] { return 21; });
auto e = cc::make_async_lazy([](int x) { return x * 2; }, once);
```

`dep->map_lazy(f)` / `dep->map_scheduled(f)` are the shorthand for the single-dependency case (mirroring the
lazy/scheduled split — there is deliberately no plain `map`). Plain non-async arguments, and depending a
`once_async` on another `once_async`, are not wired up yet.

## Polling never blocks

`poll()` drives a node forward until it completes, fails as a value, or **parks** on not-ready dependencies.
Its loop: drop ready deps, park on any that remain (requiring a cold dependency schedules it, so the
scheduler drives it), otherwise run a compute step, and on completion publish the result and wake dependents.
It never blocks a thread.

State word (atomic, CAS transitions): `cold → scheduled → running → blocked → ready`, plus
`external_pending` for manual/promise nodes. Transitions are written to be **lost-wakeup-free**: a dependency
completing and scheduling a node cannot be erased by that node parking itself.

**Subscriptions are late.** Adding a dependency does not subscribe. A node installs wakeup continuations only
at the moment it must park, and detaches them on completion. For `async<T>` the continuation list allows many
dependents; for `once_async<T>` it is capped at one (and fits the node's inline buffer, so no allocation).

## Lifetime rules

* **Frame captures** retain whatever the computation needs later — including observed `shared_async`
  dependencies. The pending-dependency list does **not** own anything; it only tracks not-ready deps for
  scheduling.
* **Owned children** (`spawn_child` / `await`) have structured lifetime under the parent frame: a child frame
  is destroyed before the parent frame, so a child may safely borrow parent-frame state by reference. Each
  node is heap-owned via `std::shared_ptr` — a child is held by its parent and thus transitively pinned by the
  root `shared_async`, and `schedule()` enqueues a `shared_ptr`, so a queued node can never be destroyed out
  from under the scheduler. This is what lets owned children be **freely scheduled** (and, later, stolen);
  nested `once_async` children work as long as some shared_async ancestor exists (it always does).

## Errors

Composition **short-circuits errors** by default: if a dependency completed with an `async_error`, the
dependent async (a `map` or a variadic dependency form) completes with that error and `f` never runs.
`async_error` also carries cancellation.

Because `cc::any_error` is move-only and a shared node's error must not be moved out, error *propagation*
currently re-materializes the message (the context chain is not preserved) — a richer error-sharing scheme is
a follow-up.

## Driving (the scheduler seam)

The async graph is **decoupled from any executor**. A worker binds a scheduler to its thread with
`async_worker_scope`; nodes reach it via `async_scheduler::current()`. The default is `inline_scheduler`: a
worker-local LIFO stack pumped on the calling thread — the shipped default, and what makes the whole system
testable without threads.

The `async_blocking_get` / `try_async_blocking_get` drivers **block the calling thread**, pumping the graph
here on an inline scheduler. They are a top-level / test convenience for self-contained graphs — never call
them from inside a frame. Their names say so deliberately.

```cpp
// convenience: drive a self-contained graph to completion on this thread (BLOCKS)
int v = cc::async_blocking_get(root);                              // asserts on error/cancel
cc::result<int, cc::async_error> r = cc::try_async_blocking_get(root);   // fallible

// lower-level, for interleaving with external completion:
cc::inline_scheduler sched;
cc::async_worker_scope scope(sched);
root->schedule();
sched.run_until([&] { return root->is_ready(); });
```

Externally produced values use a promise-style node:

```cpp
auto ext = cc::make_async_manual<int>();   // external_pending until pushed
// ... a dependent that requires ext parks ...
ext->push_value(41);                       // wakes parked dependents
```

## Zero-copy access

```cpp
std::shared_ptr<int const> v = a->try_value();        // null unless ready with a value
std::shared_ptr<cc::async_error const> e = a->try_error();
bool r = a->is_ready(); bool ok = a->has_value(); bool bad = a->has_error();
```

`try_value()` aliases the node's own `shared_ptr` onto the stored value, so it is copy-free and keeps the node
alive on its own.

## Concurrent execution: `async_thread_pool`

`cc::async_thread_pool` ([`clean-core/thread/async_thread_pool.hh`](../src/clean-core/thread/async_thread_pool.hh))
is a **work-stealing** scheduler that runs graphs on real threads. Each worker has a private LIFO deque (freshly
spawned children stay hot) and steals from siblings when idle; a shared injection queue takes work from foreign
threads and cross-affinity wakeups.

```cpp
cc::async_thread_pool pool(cc::num_hardware_threads());
cc::install_default_async_pool(pool);            // general-compute nodes now route here
auto root = build_graph();
int v = pool.blocking_get(root);                 // submit to the pool, block THIS (foreign) thread
```

`pool.blocking_get` / `try_blocking_get` submit the root and block the calling thread on a one-shot completion
hook. Call them only from a **foreign** thread — calling from inside a worker of the same pool asserts (it
would park a pool thread on its own work).

The node machinery is thread-safe under this: a per-node spinlock serializes state transitions and
continuation bookkeeping, at most one thread polls a node, a completing dependency that wakes a running node
records a re-poll instead of enqueuing a second copy, and continuations are held as `weak_ptr`s so a wake can
never touch a dependent being torn down concurrently.

### Affinity — pinning task classes to pools

An `async_affinity` is a typed `u32` bitmask of task classes. A node runs on a worker iff their masks
**overlap**; bit 0 is general-purpose compute and the default for every compute node and plain worker. Work
stealing is allowed only between overlapping masks.

Build one pool per task class and coexist them. General nodes route to the installed default pool. For a
user-defined class, create the async **lazy**, set its affinity + the route to its pool, then schedule
(affinity is frozen once scheduled — changing it asserts). The route is a raw fn pointer (allocation-free;
long-lived graphs), so the pool it names must outlive the nodes routed to it. Push/manual nodes carry the
empty mask (nothing to run).

```cpp
cc::async_thread_pool render_pool(2, cc::async_affinity{0b10}); // serves bit 1
auto t = cc::make_async_lazy([] { return render_tile(); });
t->set_affinity(cc::async_affinity{0b10}, &route_to_render_pool); // route enqueues onto render_pool
// ... schedule / require t: it now only ever runs on render_pool's workers.
```

### v1 tradeoffs (node size & locking)

The concurrency-safe node is **deliberately a v1**: correctness and a stable API first, leanness second. A
node now carries a per-node spinlock, a `_wake_pending` flag, an affinity mask, a reschedule fn pointer, a
one-shot completion hook (two words), and `weak_ptr` continuations — and it is cacheline-aligned (64 B) to
avoid false sharing. That makes `async<T>` noticeably heavier than the data it computes.

This is a known cost, not a settled design. The **semantics and the public API are the contract**; the node
layout is not. Leaner designs with the same guarantees are expected to be possible (e.g. folding the flags
into the state word, a hybrid spin-then-block lock — see the REVIEW note on `async_spinlock` — externalizing
the completion hook, or shrinking the continuation representation). These can change under the hood as the
system matures without breaking callers.

## Not yet here (follow-ups)

* **Reclaim completed owned children eagerly** rather than at root teardown (long-lived graphs hold finished
  `spawn_child` nodes until the root drops).
* A **lock-free** per-worker deque (Chase-Lev) and finer routing (worker affinity subsets within one pool);
  today the deques are mutex-guarded and every worker in a pool serves the same mask.
* Typed and **shared errors** (today error propagation re-materializes the message; the failure channel will
  grow typed errors and shared error payloads), plus cancellation propagation through a graph.
* `co_await` integration layered on top of the raw frame API; plain (non-async) arguments in the variadic
  dependency form; and depending a `once_async` on another `once_async` (nested once graphs).
