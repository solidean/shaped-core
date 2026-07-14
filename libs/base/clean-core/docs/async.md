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

The handle:

* **`shared_async<T>` = `std::shared_ptr<async<T>>`** — the normal, composable handle. Many dependents may
  observe it. `async<T>` itself is non-copyable and immovable; you copy the `shared_ptr`, never the node.
  (clean-core has no shared pointer yet, so `std::shared_ptr` is used deliberately, as in `pinned_data`.)

### The raw compute frame

A frame is a callable `async_step_status(async_context&)`: it resolves its outcome **through** the context and
returns a status. It may be a hand-written state machine that adds dependencies dynamically as it runs:

```cpp
auto a = cc::make_async_lazy<int>(
    [step = 0, child = cc::shared_async<int>()](cc::async_context& actx) mutable -> cc::async_step_status
    {
        switch (step++)
        {
        case 0:
            child = cc::make_async_lazy([] { return 10; }); // a dependency created on the fly
            actx.require(child);
            return actx.wait_for_dependencies();
        default:
            return actx.resolve_to_value(*child->value_ptr() + 5); // or actx.success(...)
        }
    });
```

A raw frame's return carries no value type (it returns a status), so its node must give `T` explicitly —
`make_async_lazy<int>(...)`. A plain value-returning frame (`[](int x){ return x + 1; }`) deduces `T`.

`async_context` gives a frame:

* `require(dep) -> bool` — true if `dep` is already ready (read its value now); otherwise records it as a
  pending dependency and returns false. **No subscription happens here.** `dep` may be a `shared_async`
  created earlier or one the frame builds on the fly (dynamic dependencies) — capture it so it stays alive.
* resolve the result — each returns the matching status, so `return actx.xxx(...)`: `resolve_to_value(v)` /
  `resolve_to_error(async_error | any_error)` (aliased `success(v)` / `error(...)`), plus
  `wait_for_dependencies()` and `yield()`. `resolve_to_value` stores the value into the node as the frame
  runs; `resolve_to_error` hands the failure to the completion path.

A frame is **re-entrant**: one that waits is re-polled once its dependencies are ready. A typical two-phase
frame (register deps → `wait`, then compute) therefore runs twice. It is never entered again after it
produces a value or error — the frame is destroyed the moment it completes.

### Composition without hand-writing a frame

Most compositions don't need a raw frame. `make_async_lazy` / `make_async_scheduled` are **variadic in their
dependencies**: extra arguments are `shared_async`s that are awaited and
**unwrapped** to plain values before `f` runs, with errors short-circuiting (the first failed dependency
propagates and `f` is skipped). `f` may take a leading `async_context&` or omit it entirely — the wrapper
adapts either way, and a no-dependency `f` may drop the context too.

```cpp
auto a = cc::make_async_scheduled<int>(/* ... */);
auto b = cc::make_async_scheduled<int>(/* ... */);
auto c = cc::make_async_lazy([](int x, int y) { return x + y; }, a, b);   // c waits for a,b; f gets plain ints
auto d = cc::make_async_lazy([] { return 7; });                          // no deps, no context
```

The single-dependency transform is just the one-argument form: `make_async_lazy(f, dep)` /
`make_async_scheduled(f, dep)` (lazy vs scheduled — there is deliberately no plain `map` that hides which).
Plain non-async arguments in the variadic dependency form are not wired up yet.

## Polling never blocks

`poll()` drives a node forward until it completes, fails as a value, or **parks** on not-ready dependencies.
Its loop: drop ready deps; if any remain, **drive one inline, depth-first** — `require()` already made every
dependency runnable, so the poller descends into a not-ready dependency's own `poll()` on the current stack
(bounded by a per-worker depth cap) and re-evaluates. Only when a dependency cannot be completed inline — a
manual/push node, one already running on another worker, or the depth cap — does it fall back to installing
wakeup continuations and parking. Otherwise it runs a compute step, and on completion publishes the result and
wakes dependents. It never blocks a thread.

**Execution order among a node's dependencies is unspecified.** The eager drive visits them in an unspecified
order and a work-stealing pool may complete them in any order; only the resulting *values* are guaranteed. The
native stack is bounded by the depth cap, not by graph depth (past the cap the loop parks instead of recursing).

State word (atomic, CAS transitions): `cold → scheduled → running → blocked → ready`, plus
`external_pending` for manual/promise nodes. Transitions are written to be **lost-wakeup-free**: a dependency
completing and scheduling a node cannot be erased by that node parking itself.

**Subscriptions are the exception, not the rule.** Adding a dependency does not subscribe, and the eager
depth-first drive completes most dependencies inline without ever parking. A node installs wakeup continuations
only when it actually has to park — on a manual/push dependency, one running on another worker, or when the
inline depth cap is hit — and detaches them on completion. The continuation list allows many dependents (a
single dependent fits the node's inline buffer, so no allocation).

## Lifetime rules

* **Frame captures** retain whatever the computation needs later — including observed `shared_async`
  dependencies (a dependency the frame builds on the fly must be captured so it stays alive). The
  pending-dependency list does **not** own anything; it only tracks not-ready deps for scheduling.
* Each node is heap-owned via `std::shared_ptr`, and `schedule()` enqueues a `shared_ptr`, so a queued node
  can never be destroyed out from under the scheduler — which is what lets a required dependency be **freely
  scheduled** (and, later, stolen) while its dependents hold it alive.

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
threads and cross-thread wakeups.

```cpp
cc::async_thread_pool pool(cc::num_hardware_threads());
cc::install_default_async_pool(pool);            // compute nodes now route here when off-worker
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

### Routing to a specific pool

There is no task-class / affinity system: every worker in every pool serves all compute work, and steals are
always eligible. A node with no active worker scope and no explicit target routes to the installed **default**
pool. To drive a graph on a *specific* pool, submit its root there — `pool.blocking_get(root)` (or the
lower-level `root->schedule_on(pool)`) — rather than pinning the node. Build and coexist as many pools as you
like; only one may be the process-wide default at a time.

(An earlier version pinned nodes to pools via a typed `async_affinity` bitmask + a per-node reschedule fn
pointer. That was removed to shrink the node; if per-class routing returns it belongs on the scheduler, not as
bytes on every node.)

### v1 tradeoffs (node size & locking)

`sizeof(async<int>)` is **64 B — one cache line** (down from an original 384 B), and the node is
cacheline-aligned to avoid false sharing between unrelated nodes. Three ideas get it there:

* The value, the failure-channel error, and the set of dependents to wake (continuations) are **mutually
  exclusive over a node's life** — continuations matter only before completion, the value/error only after —
  so the error and the continuation head share one 16 B result slot, discriminated by the node state (see
  `async_result_slot`); the value lives separately in the typed node. The slot holds **one inline weak
  dependent** (the common single-dependent case pays no allocation) and spills the rest, plus any one-shot
  completion latch, into slab-backed cells. Completion steals the continuation head under the node lock, then
  moves the error into the freed slot and publishes `ready` last, so a late subscriber never observes `ready`
  before the result is in place.
* The compute frame is a **one-pointer `unique_function`** (the closure and its type-erased ops share one
  node; see `cc::poly_node_allocation`).
* The cacheline alignment lives on the concrete typed node, not the untemplated base, so the base does not
  round its own size up to 64 and push the value/frame onto a second line.

This is still a v1 on locking: each node carries a per-node spinlock and a `_wake_pending` flag plus the
vtable pointer. The **semantics and the public API are the contract**; the node layout is not. Leaner locking
is possible (folding the flags into the state word, a hybrid spin-then-block lock — see the REVIEW note on
`async_spinlock`), and can change under the hood as the system matures without breaking callers.

## Not yet here (follow-ups)

* **Structured/owned children** — a parent frame spawning children with borrow-by-reference lifetime. An
  earlier `spawn_child` / `await` subsystem was removed pending a real use case; fork/join today uses regular
  refcounted `shared_async` children captured and required by the parent frame.
* A **lock-free** per-worker deque (Chase-Lev) and finer per-worker routing within one pool; today the deques
  are mutex-guarded and every worker serves all work.
* Typed and **shared errors** (today error propagation re-materializes the message; the failure channel will
  grow typed errors and shared error payloads), plus cancellation propagation through a graph.
* `co_await` integration layered on top of the raw frame API, and plain (non-async) arguments in the variadic
  dependency form.
