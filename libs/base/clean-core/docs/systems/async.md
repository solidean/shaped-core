# cc::async — a value/dataflow async system

`cc::async<T, E = async_error>` is a low-overhead async for **compute-heavy dependency graphs**.
The model is values and dataflow transformations, not futures/promises or callback chains.
It is the CPU fan-out "task system" that `cc::threaded_actor` defers to.
`E` is the failure-channel type, defaulting to `async_error` — a move-only wrapper over `cc::any_error`; any type works (an enum, a small struct, …).

Headers: [`async.hh`](../../src/clean-core/thread/async.hh) (public, templated) and [`async_node.hh`](../../src/clean-core/thread/async_node.hh) (untemplated core + scheduler seam).
[`async_coroutine.hh`](../../src/clean-core/thread/async_coroutine.hh) is the opt-in `co_await` layer over them — see [co_await / co_return](#co_await--co_return).
**Incubator-stage** API — expect it to grow and change.

## Model

An `async<T, E>` is an eventual `result<T, E>` produced by a **compute frame**: a callable or state machine polled through an `async_context<T, E>`.
Failure and cancellation are **values** — the default `async_error` carries both — not exceptions or out-of-band control flow.
An exception that escapes a frame is *converted* into one of those values rather than propagating.
So a node is a value/error machine from the outside, whatever runs inside it — see [Exceptions escaping a frame](#exceptions-escaping-a-frame).

```cpp
auto a = cc::make_async_scheduled<int>([](cc::async_context<int>&) { return 40; });
auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);   // b depends on a; f gets a plain int
int v = cc::async_blocking_get_singlethreaded(b);   // drives the graph on this thread -> 42
```

The handle:

* **`shared_async<T, E>` = `cc::shared_ptr<async<T, E>>`** — the normal, composable handle: 8 B, an intrusive refcount over one slab node.
  Many dependents may observe it.
  `async<T, E>` itself is non-copyable and immovable — you copy the handle, never the node.

### The raw compute frame

A frame is a callable `async_step_status(async_context<T, E>&)`: it resolves its outcome **through** the typed context and returns a status.
It may be a hand-written state machine that adds dependencies as it runs:

```cpp
auto a = cc::make_async_lazy<int>(
    [step = 0, child = cc::shared_async<int>()](cc::async_context<int>& actx) mutable -> cc::async_step_status
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

A raw frame returns a status, not a value, so its node must give `T` explicitly: `make_async_lazy<int>(...)`.
A plain value-returning frame (`[](int x){ return x + 1; }`) deduces `T` context-free.
A value frame that *also* takes a context must give `T` explicitly.

`async_context<T, E>` gives a frame:

* `require(dep) -> bool` — true if `dep` is already ready, so read its value now; otherwise it records `dep` as pending and returns false.
  **It neither subscribes nor schedules** — the poll loop owns both, see [publish all-but-one](#publish-all-but-one).
  `dep` may be a `shared_async` created earlier or one the frame builds on the fly (dynamic dependencies) — capture it so it stays alive.
* resolve the result — `resolve_to_value(v)` / `resolve_to_error(e)`, aliased `success(v)` / `error(...)`, plus `wait_for_dependencies()` and `yield()`.
  Each returns the matching status, so the shape is `return actx.xxx(...)`.
  For the default `E`, `error(any_error)` wraps.
  Both resolves complete the node **in place** as the frame runs, and take their argument **by value** — anything convertible to `T` / `E` works, converted at the call site.
* **emplace resolves** — `resolve_to_value_emplace(args...)` / `resolve_to_error_emplace(args...)` build the payload **in place** from `args`, never moved, so an **immovable `T`** works.
  The by-value `resolve_to_value` requires `T` to be nothrow-move-constructible; the emplace form does not.
  `args` are forwarded by reference into the payload slot, and resolving destroys the frame first.
  So they **must not reference the frame's own captures** — nor anything only those captures keep alive, such as a dependency's value.
  The by-value resolves have no such caveat.

A frame is **re-entrant**: one that waits is re-polled once its dependencies are ready.
A two-phase frame (register deps → `wait`, then compute) therefore runs twice.
Its state persists across polls untouched — the frame is never moved, so a `mutable` closure picks up where it left off.

**Resolving is terminal, in the `delete this;` sense.**
The frame lives inline in the node and the value is built over its slot, so a resolve destroys the closure *while it is still running*, then builds the result there.

> **A frame must not touch its captures, or the context, after calling a `resolve_*` action.**
> `return actx.success(v);` is the shape — a tail resolve touches nothing afterwards.

The resolve's *arguments* are fine: they are evaluated before the call, and the by-value resolves copy into a stack temporary first.
Resolving twice trips an assert.

### Composition without hand-writing a frame

Most compositions don't need a raw frame.
`make_async_lazy` / `make_async_scheduled` are **variadic in their dependencies**: extra arguments are `shared_async`s, awaited and **unwrapped** to plain values before `f` runs.
Errors short-circuit — the first failed dependency propagates and `f` is skipped.
`f` may take a leading `async_context&` or omit it entirely; the wrapper adapts either way.

```cpp
auto a = cc::make_async_scheduled<int>(/* ... */);
auto b = cc::make_async_scheduled<int>(/* ... */);
auto c = cc::make_async_lazy([](int x, int y) { return x + y; }, a, b);   // c waits for a,b; f gets plain ints
auto d = cc::make_async_lazy([] { return 7; });                          // no deps, no context
```

The single-dependency transform is just the one-argument form, `make_async_lazy(f, dep)` / `make_async_scheduled(f, dep)`.
There is deliberately no plain `map` that hides which of the two you get.
Plain non-async arguments in the variadic dependency form are not wired up yet.

## Polling never blocks

`poll()` drives a node forward until it completes, fails as a value, or **parks** on not-ready dependencies.
It never blocks a thread.

The loop drops ready deps; if any remain, it **drives one inline, depth-first** — descending into that dependency's own `poll()` on the current stack — and re-evaluates.
A cold node polls fine, which is what lets `require()` stay out of scheduling.
Only when a dependency cannot be completed inline does it schedule the remaining deps, install wakeup continuations and park.
Three things cause that: a manual/push node, one already running on another worker, or the depth cap.
Otherwise it runs a compute step, and on completion publishes the result and wakes dependents.

**Execution order among a node's dependencies is unspecified** — only the resulting *values* are guaranteed.
The native stack is bounded by the depth cap, not by graph depth: past the cap the loop parks instead of recursing.

State word (atomic, CAS transitions): `cold → scheduled → running → blocked → ready`, plus `external_pending` for manual/promise nodes.
Transitions are **lost-wakeup-free**: a dependency completing and scheduling a node cannot be erased by that node parking itself.

**Subscriptions are the exception, not the rule.**
Adding a dependency does not subscribe, and the depth-first drive completes most dependencies inline without ever parking.
A node installs wakeup continuations only when it actually has to park, and detaches them on completion.
Many dependents are allowed; a single dependent fits the node's inline buffer, so it costs no allocation.

## Lifetime rules

* **Frame captures own what the computation needs later**, including the `shared_async` dependencies it observes.
  A dependency the frame builds on the fly must be captured, or nothing keeps it alive.
  The pending-dependency list owns **nothing** — it only tracks not-ready deps for scheduling.
* Each node is heap-owned via `cc::shared_ptr` (8 B, intrusive — see the node layout below), and `schedule()` enqueues a `shared_ptr`.
  A queued node therefore can never be destroyed out from under the scheduler, which is what lets a required dependency be freely scheduled — and later stolen — while its dependents hold it alive.

## Errors

Two layers, two contracts:

* **The `make_async_*` sugar auto-propagates.** A dependency that completed with an error completes the dependent with that error, and `f` never runs.
  The sugar assumes a **single failure type `E`** across the graph: a propagated error must be constructible into the dependent's `E`.
  `async_error` also carries cancellation.
* **A raw compute frame does NOT auto-propagate — the frame decides.**
  A dependency that resolved to an error still counts as *ready*, so the frame is re-run.
  It must check `dep->try_error()` itself and choose to propagate, transform or ignore:

  ```cpp
  if (!ctx.require(dep)) return ctx.wait_for_dependencies();
  if (auto const* e = dep->try_error()) return ctx.resolve_to_error(/* map *e to this node's E */);
  return ctx.resolve_to_value(*dep->try_value());
  ```

**Propagation strategy (`impl::async_error_propagate`).** A shared node's `any_error` must never be moved out, so copying an error out of one uses a per-`E` hook.
A copyable custom `E` is **copied**; the default move-only `async_error` is **re-materialized from its message**, which does not preserve the context chain.
Heterogeneous-`E` propagation is not wired into the sugar — bridge it by hand in a raw frame.

### Exceptions escaping a frame

**A frame that throws instead of resolving fails its own node on `E`.**
`poll()` catches it, and the node reaches `ready_error` through the same transition `resolve_to_error` would have made.
The frame is destroyed, its captures released, dependents woken, dependencies unsubscribed.
The error's kind is always `error`, never `cancelled` — cancellation is a deliberate cooperative outcome, an exception is a failure.

Containment is not politeness.
Left to escape, the exception would leave the node `running` forever, parking every dependent permanently, and would unwind out of a pool worker's thread function straight into `std::terminate`.

The default `async_error` carries the message: a `std::exception`'s `what()`, or a fixed text for anything else.
A custom `E` opts in by specializing the trait, and one that does not is a **runtime** diagnostic — an assert, then the pre-existing rethrow — never a compile error:

```cpp
template <>
struct cc::custom::async_error_from_exception_trait<my_err>
{
    static my_err make(cc::string_view message) { return my_err{cc::string(message)}; }
};
```

Two corollaries of the terminal-resolve rule above:

* **A frame must not throw after resolving.** The frame is gone and the node may already be freed, so there is nothing left to fail; the exception is dropped, with an assert.
* Throwing is a *legal* way to fail, not the cheap one — it costs an unwind plus a message allocation, against an in-place construct for `resolve_to_error`.

Not covered: an allocation failure inside the poll machinery itself, and, on MSVC, SEH faults such as an access violation.
The handler is C++ EH only, which is what keeps a hardware fault from being swallowed as an error value.

## Driving (the scheduler seam)

**You never block on an async — a scheduler makes progress on it**, and blocking is a convenience that scheduler offers.
The graph is **decoupled from any executor**: a worker binds a scheduler to its thread with `async_worker_scope`, and nodes reach it via `async_scheduler::current()`.

There are two schedulers, and they present the same surface:

| | drives | publishes work | use |
|---|---|---|---|
| `singlethreaded_scheduler` | inline, on the calling thread | never | tests, debug, deterministic runs |
| `async_thread_pool` | worker threads **plus the calling thread** | yes | real concurrent work |

```cpp
cc::singlethreaded_scheduler sched;
int v = sched.blocking_get(root);        // drives + blocks THIS thread
cc::async_thread_pool pool;              // defaults to hardware concurrency - 1 (the caller is the other thread)
int v = pool.blocking_get(root);         // the calling thread PARTICIPATES, then blocks
```

`async_thread_pool::blocking_get` does not hand the graph over and park.
The calling thread borrows a pool slot and runs the graph itself, stealing like any worker, and parks only once nothing is left for it.
A graph that never forks therefore costs tens of nanoseconds rather than a cross-thread round trip, and a large one still spreads across the pool — the caller's deque is stealable like any other.
That is why the default worker count is one *fewer* than the hardware concurrency, and why a 1-worker pool still publishes work: its caller is a second participant.

`singlethreaded_scheduler` is single-threaded **by construction, not by circumstance**.
It has no peers, so it never publishes work and a graph's nodes cannot run concurrently however many cores sit idle.
That is what makes the whole system testable without threads.

`async_no_worker_scope` is the other direction: for its lifetime the calling thread has **no** scheduler bound.
That is an ordinary state — a foreign thread has never had one — and the scope only makes it reachable from inside a worker.

It is what a host driving foreign code inside its own graph needs.
Left bound, a node that code schedules lands in the *host's* queue and is run later, outside the lifetime of everything its frame captured.
Nexus unbinds around every test body for exactly that reason ([parallel-execution](../../../nexus/docs/parallel-execution.md)).
A node created inside the scope still routes to the installed default pool, exactly as it would on a thread that never had a scheduler.

For a self-contained graph, the free functions build a throwaway scheduler for you.
The verbose names are deliberate — this is a test/debug convenience, not how real work gets scheduled:

```cpp
int v = cc::async_blocking_get_singlethreaded(root);                       // asserts on error/cancel/no-progress
cc::optional<cc::result<int, cc::async_error>> r = cc::try_async_blocking_get_singlethreaded(root); // fallible
```

The `try_` form returns an **optional** result.
`nullopt` means the scheduler pumped everything reachable from here and `root` is still not ready — see [Multi-scheduler correctness](#multi-scheduler-correctness).
`blocking_get` asserts on that outcome, and on an error, so keep it for graphs you know complete inline.

`run_one` / `run_until` / `drain` are the underlying pump, and the pump is what you need when a graph parks on a manual node.
Nothing progresses while you are not inside it, so call it again after the external push.

```cpp
cc::singlethreaded_scheduler sched;
cc::async_worker_scope scope(sched);
root->schedule();
sched.run_until([&] { return root->is_ready(); }); // interleave an external push here, then pump again
```

### Publish all-but-one

A node's poll loop drives one dependency inline on its own stack, so enqueuing *that* one would be pure churn.
It would be popped later as a ready no-op, and until then the queue's strong handle would pin it alive.
The loop therefore publishes only the **other** dependencies, and only when `async_scheduler::has_steal_capable_peers` says someone could actually claim them.
`require()` neither schedules nor subscribes: the poll loop owns both, and schedules whatever is left before it parks.

For a `singlethreaded_scheduler` chains and single-dependency transforms enqueue **nothing at all**.
For a pool, a fan-out of n publishes n−1 stealable siblings, at every worker count — a 1-worker pool is still steal-capable, because the participating `blocking_get` caller is a second claimant.

This is a lifetime property as much as a performance one: a queued entry is a strong node handle, so work abandoned in a queue pins its graph alive.
The `async - a reused singlethreaded_scheduler settles empty after each graph` test pins that.

A published sibling costs **two refcount atomics**: `route_after_schedule`'s `from_alive`, and the worker dropping the handle after `poll()`.
The deque round-trip itself has none — an entry is a raw pointer whose strong count is moved in and out by `cc::shared_ptr`'s `release` / `adopt`, a load and a store rather than an RMW.
Those two are inherent to a queued entry co-owning its node, so removing them means changing node lifetime, not the scheduler.

### Multi-scheduler correctness

A node or subgraph **may be reached from more than one scheduler at once**.
An outer API alternating single- and multi-threaded computation over shared asyncs visits one subtree from a `singlethreaded_scheduler` and an `async_thread_pool` concurrently.
This is supported and must stay correct.
It is **not optimized for** — performance for a genuinely shared subgraph is not a goal, only correctness.

**Guaranteed under concurrent scheduling:** no data race and no double-compute, whichever scheduler runs a node.
At most one thread polls it (`try_begin_running`), and a per-node spinlock serializes state transitions and continuation bookkeeping.
A dependency completing while the node runs records a re-poll rather than enqueuing a second copy.
Continuations are held as `weak_ptr`s, so a wake can never touch a dependent being torn down.

**Not guaranteed: which scheduler runs a node.**
`has_steal_capable_peers` is read from the *current thread's* scheduler.
A `singlethreaded_scheduler` driving a subtree inline therefore forces it single-threaded even where a pool could have parallelized it.
A node can also **migrate mid-flight**: `route_after_schedule` sends a woken dependent to whatever scheduler is bound on the **waking** thread.
A graph driven from an st scheduler can therefore finish on a pool, or the reverse.

That migration is why `singlethreaded_scheduler::try_blocking_get` returns an **optional**.
A drained queue does not mean the graph is stuck, only that *this* scheduler cannot advance its root.
It may be parked on an unpushed manual node, or have migrated onto another scheduler that is still driving it.
`nullopt` is "not from here, not yet": push and retry, or let the owning scheduler finish.
An st scheduler cannot assume it will ever see every node of "its" graph, so it reports rather than asserts.

**`try_blocking_get` / `blocking_get` drain their queue before returning**, with the worker scope still bound.
Migration runs the other way too: a node parked on a pool can be woken on an st thread and enqueued onto the st scheduler, which then stops pumping once *its* own root is ready.
Without the drain that node sits `scheduled` in a queue nobody drives.
`schedule()` / `schedule_on()` are idempotent on `scheduled`, so no other scheduler can reclaim it and a `pool.blocking_get` on it would hang.
The drain settles any migrated-in node instead: completed, or re-parked as `blocked` and re-woken later onto whichever scheduler finishes its dependency.
It runs that work single-threaded on the returning thread, the same single-threading a shared subtree already incurs here.
The drain is a no-op in the common case: publish-all-but-one leaves the queue empty once the root is ready.

Externally produced values use a promise-style node:

```cpp
auto ext = cc::make_async_manual<int>();   // external_pending until pushed
// ... a dependent that requires ext parks ...
ext->push_value(41);                       // wakes parked dependents
```

## Zero-copy access & consuming

```cpp
int const* v = a->try_value();               // non-owning; null unless ready with a value
cc::async_error const* e = a->try_error();   // non-owning, typed E const*; null unless ready with an error
bool r = a->is_ready(); bool ok = a->has_value(); bool bad = a->has_error();
```

`try_value()` / `try_error()` return non-owning pointers **into** the node's payload — copy-free, and stable for as long as your handle keeps the node alive.

To **move** the outcome out instead of reading it in place, consume the handle:

```cpp
cc::result<int, cc::async_error> r = cc::into_result(cc::move(a));  // a must be ready; MOVES value/error out
```

`into_result` takes the handle by value and moves the payload out into a `cc::result<T, E>`.
It moves out of shared node storage, so **any other live handle's later `try_value()` / `try_error()` reads a moved-from value** — use it only when you are done with the async.
`T` must be move-constructible; an immovable `T` is a compile error by design, so read it in place via `try_value()`.

### Born-ready factories

For a value/error known up front, skip the frame and scheduling entirely:

```cpp
auto rv = cc::make_async_from_value(42);                          // ready_value, drivable as a dependency
auto re = cc::make_async_from_error<int>(async_error::make_cancelled());
auto ri = cc::make_async_from_value_emplace<Immovable>(7);        // build T in place (immovable T ok)
// also make_async_from_error_emplace<T, E>(args...)
```

## `co_await` / `co_return`

A coroutine **is** a compute frame.
The frame contract — re-entrant, polled repeatedly, never moved, resolving through the context — is one-for-one what a coroutine gives you, so the layer in
[`async_coroutine.hh`](../../src/clean-core/thread/async_coroutine.hh) adds **no node state at all**.

| frame contract | coroutine |
|---|---|
| polled again once dependencies are ready | `h.resume()` |
| state persists, frame never moved | the coroutine frame is stable heap storage |
| `require(dep)` then `wait_for_dependencies()` | `await_suspend` requires, then suspends |
| `resolve_to_value` | `co_return` |
| exception containment | `unhandled_exception()` |

Including that header is what makes a function returning `shared_async<T, E>` a coroutine — `async.hh` alone does not, and `<coroutine>` never reaches it.
A graph that does not await pays nothing: no field on the node, no branch in `poll()`.

```cpp
cc::shared_async<int> load(cc::string path)      // eager: scheduled at its initial suspend
{
    auto const& bytes = co_await read(path);     // a failed read short-circuits
    co_return parse(bytes);
}
```

**`co_await` never starts work — creation does.**
`require()` is a wakeup edge; parallelism comes from a node being `scheduled` and stealable.
So `co_await a; co_await b;` serializes nothing as long as `a` and `b` were created eagerly, which is the default a coroutine gives you.
Eager scheduling happens at the **initial suspend**, not in `get_return_object` — the coroutine has to be fully suspended before the node can be handed to a peer.
That is also why laziness lives in the return type: by the time the caller holds the handle, the choice is already made.
`cc::async_lazy<T, E>` is the cold twin, and converts to `shared_async<T, E>`.

`cc::async_all` is for the other case, where the fan-out is built **at** the await site.
It requires every dependency before parking, so a fan-out parks once on all of them rather than walking them in sequence.
It hands back nothing: read each value with a plain `co_await`, which no longer suspends once the node is ready.

```cpp
co_await cc::async_all(a, b, c);          // one park, on all three
auto const sum = co_await a + co_await b; // neither suspends
```

### Failure short-circuits without unwinding

A failed dependency means the coroutine is **never resumed**.
The frame destroys it while suspended — which runs the destructors of every in-scope local — and resolves the node with the propagated error.
The rest of the body is skipped, and so is any `catch` in it.
That matches the `make_async_*` sugar's short-circuit, and it avoids both an unwind and the message re-materialization a throwing `await_resume` would cost the default `async_error`.

Three writers share **one** failure slot on the promise — a dependency short-circuit, an escaped exception, and `cc::async_fail` — and the frame is the only reader.
`co_await cc::async_settled(a)` waits without short-circuiting, and leaves you to read `a->try_value()` / `a->try_error()`.
`cc::async_as_result(a)` is the same wait handed back as a `cc::result`, at the cost of a copy.

`co_await cc::async_fail(e)` is the uniform failure spelling.
`co_return cc::error(...)` also works, but only for a non-`unit` `T`.
`return_void` and `return_value` cannot coexist, so a `cc::unit` coroutine keeps the bare `co_return;` and fails through `async_fail`.

### What it costs

One heap allocation for the coroutine frame, on top of the node.
It cannot be elided, since the handle escapes into the node, and it goes through the same slab the node does (`promise_type::operator new`).

The node's stored frame is that one handle: **8 B, always inline**, so a coroutine never spills into the boxed `cc::unique_function`.
The useful comparison is therefore *a lambda frame that spills* — above the 24 B budget a closure boxes anyway, so at that size a coroutine costs the same.
Below it, the small lambda frame remains the zero-allocation path, deliberately.

### The sharp edges

* **Coroutine parameters are captured by their declared type**, so a reference parameter dangles across the first suspend.
  Take them **by value**.
* **`T` must be movable** — `co_return` moves the result through the promise.
  An immovable `T` stays on the raw-frame emplace API.
* **A coroutine can resume on a different thread than it suspended on.**
  Nothing may be held across a `co_await` that is bound to a thread.
* **Dropping an eager coroutine's handle does not cancel it**: the schedule queue holds the node, and the system is cooperative throughout.
* `co_await a` yields a `U const&` **into the node's payload**, so reading it copies nothing — but binding `auto const&` to the result of awaiting a *temporary* dangles once the full-expression ends.

## Concurrent execution: `async_thread_pool`

`cc::async_thread_pool` ([`clean-core/thread/async_thread_pool.hh`](../../src/clean-core/thread/async_thread_pool.hh))
is a **work-stealing** scheduler that runs graphs on real threads.
Each worker owns a lock-free **Chase-Lev deque** ([`impl/chase_lev_deque.hh`](../../src/clean-core/thread/impl/chase_lev_deque.hh)) and pushes/pops its own bottom end LIFO.
Freshly spawned children therefore stay hot, and the common path takes no cross-thread sync at all.
Idle workers steal from the top of a *randomly chosen* sibling's deque, which is the only place threads meet.
A shared, mutex-guarded injection queue takes work from foreign threads.
It is deliberately not lock-free: only genuinely foreign submits reach it — a worker waking a node enqueues locally — so it is cold by construction.

```cpp
cc::async_thread_pool pool(cc::num_hardware_threads());
cc::install_default_async_pool(pool);            // compute nodes now route here when off-worker
auto root = build_graph();
int v = pool.blocking_get(root);                 // submit to the pool, block THIS (foreign) thread
```

`pool.blocking_get` / `try_blocking_get` drive `root` on the calling thread, which borrows a pool slot and participates (see above), and block only once there is nothing left for it to run.
With every external slot already claimed they fall back to submitting the root and blocking on a one-shot completion hook.
Call them only from a **foreign** thread; from inside a worker of the same pool it asserts, since it would park a pool thread on its own work.

The node machinery is thread-safe under this — see [Multi-scheduler correctness](#multi-scheduler-correctness) for exactly what is and is not guaranteed.

### Without threads

On a platform with no OS threads — WebAssembly, or any build configured `-DSC_THREADS=OFF` — the pool does **not** disappear.
Gating the type on `CC_HAS_THREADS` would push the branch into every caller, so it keeps its whole API and falls back instead.
No threads are started, `worker_count()` is 0, the constructor's count is ignored, and `enqueue` / `submit` push onto a LIFO queue that `blocking_get` pumps on the calling thread.

One decision produces all of that: constructing `async_scheduler(false)` — **no steal-capable peers**.
The poll loop publishes a node's dependencies only for peers to steal, so with none it drives the whole graph inline on the caller's stack and the queue stays empty on the common path.
Nothing to steal means no deques, nobody to wake means no wake protocol, nobody to park behind means no condvar.
What is left is `singlethreaded_scheduler`'s behavior reached through the pool's API, with the node's spinlock compiled away — the only thread that could hold it is the one asking for it.

The one thing it cannot do is **wait**.
A graph parked on work only another thread could deliver never completes, and `blocking_get`'s `is_ready()` assert reports that rather than hanging.
That is the same boundary `singlethreaded_scheduler::try_blocking_get` draws with its nullopt.

The pool's own tests ([async-pool-test.cc](../../tests/thread/async-pool-test.cc)) run in **both** modes; only the ones that genuinely need a second thread are gated.

### The hot path costs no shared RMW

The design rule is **no MPMC contention when nobody is actively stealing**, and what matters there is **coherence traffic, not instruction count**.
A `fence(seq_cst)` drains *this core's* store buffer: it touches no memory and invalidates nothing, so N cores fencing cost O(1) each.
A `lock xadd` on a shared counter needs the line **Exclusive**, so it invalidates every other core's copy and ping-pongs — O(N).

Publishing a node therefore compiles to ~15 instructions with **zero refcount atomics** and exactly one locked instruction — the fence, which clang lowers to a `lock inc` on a **private stack slot**.
The `_sleepers` check that follows is a relaxed load of a shared, read-mostly line, and the whole wake path is branched over when nobody is asleep.

The deque holds **raw node pointers**, not handles: a Chase-Lev slot is read speculatively by thieves that may lose the race for it, so it cannot hold a smart pointer at all.
Each entry owns one strong count by hand, via `cc::shared_ptr`'s `release` / `adopt` pair.
**The pool therefore owes every queued entry a release** — `~async_thread_pool` drains its deques after joining.
Without that, abandoning a 131k-node graph leaks ~49k nodes, which the `async - destroying a pool releases work abandoned in its deques` test pins.

**Wake protocol.** There is deliberately no counter of claimable work.
A worker's scan of the deques already answers "is there work", so a counter would be a hot-path RMW serving a cold-path question.
It is a Dekker store-load cross-pairing instead, and both sides pay:

* the producer's push ends in a **relaxed** `_bottom` store, so it fences before loading `_sleepers`;
* a would-be sleeper does a seq_cst RMW on `_sleepers`, then **re-scans** before committing to the condvar.

Seq_cst gives one total order over the two, so at least one side sees the other: either the sleeper finds the work, or the producer sees the sleeper and notifies.
The producer still passes through the wait mutex on the wake path — only `wait()`'s atomic release-and-enqueue closes the check-then-wait window, so an epoch counter cannot remove it.
`_wake_epoch` is the condvar predicate: monotonic, and touched only when a sleeper exists.

Workers **spin ~64 rounds before sleeping** — a condvar round-trip is ~1–10 µs against fork-join tasks that cost ~100 ns, so this is worth ~25% on a spawn tree rather than being a micro-optimization.
Steal victims are **randomized** with bounded attempts: a linear scan points every idle worker at worker 0, which is both a contention hotspot and unfair.

### Measured

Scaling is pool-at-P vs pool-at-1-worker, so it is independent of leaf cost.
Judge it against the machine's **performance** cores rather than its thread count — the curve bends at the E-core and SMT boundaries by design.
On an i9-12900H (6P+8E) the five fork-join shapes land between **4.51x and 6.08x**, the reduction at the top and the parallel-for transform at the bottom.
The spawn tree is the pure-overhead metric — its leaves do nothing, so its ns/node *is* the pool's per-node cost, and it falls to **12.2 ns/node** across the worker sweep.
[benchmarks/async-benchmark](../benchmarks/async-benchmark.md) owns the full table, the method behind it, and which columns of a run survive a throttling laptop.

### Routing to a specific pool
There is no task-class or affinity system: every worker in every pool serves all compute work, and steals are always eligible.
A node with no active worker scope and no explicit target routes to the installed **default** pool.
To drive a graph on a *specific* pool, submit its root there — `pool.blocking_get(root)`, or the lower-level `root->schedule_on(pool)` — rather than pinning the node.
Build and coexist as many pools as you like; only one may be the process-wide default at a time.

### Node layout (size & locking)

A node is a **16 B header** followed by a payload slot; `async<int>` is **64 B — one cache line**, cacheline-aligned so unrelated nodes never share a line.

* **16 B header** — one `atomic<u64>` intrusive refcount plus one `atomic<u64>` control word.
  The refcount is fused, strong in the high half and weak in the low half, so a handle is one pointer with no separate control block.
  Fusing is what lets the last strong drop test both halves with a **single acquire load** and skip both locked RMWs when it is the sole owner (`cc::fused_refcount`, see [Cost](#cost)).
  The control word is a **tagged pointer**: a 32-aligned `async_type_ops const*` in the high bits, and the lifecycle state + wake-pending flag + spinlock bit in the low 5 bits.
  There is **no C++ vtable** — `async_type_ops` is a static-constexpr descriptor carrying the typed value/error destructors, the inline frame's invoke/destroy, and the node's size class.
  It is keyed on `(size class, value-teardown, error-teardown, frame-invoke, frame-destroy)` rather than on `(T, E)`, so it **collapses**.
  A trivially-destructible type uses a null teardown.
  Every *frameless* async — manual/push, born-ready — whose payloads land in the same size class with the same teardowns shares one instance.
  `async<int, async_error>` and `async<float, async_error>` therefore get the same pointer.
  A framed descriptor is one instance per closure, which costs nothing: each closure already emits its own code.
  `is_ready()` / `is_cold()` are lock-free acquire loads of the word.
  Both words are `cc::atomic`, so a no-threads build gets plain loads and stores here and the spinlock bit compiles away entirely — nothing can contend it (see [Without threads](#without-threads)).
* **Payload slot (offset 16), one hand-managed union.**
  The unresolved arm and the compute frame are **mutually exclusive with** the resolved value ⊍ error.
  The arm matters only before completion and the value/error only after, so they share the slot, discriminated by the node state.
  The 24 B arm is **ambient 8 + deps 8 + conts 8**, and the compute frame is the payload **tail** immediately after it.
  Those three sit at fixed payload offsets 0/8/16, so the untyped base reaches all of them — and the frame — by constant offset.
  The value is built **straight into the slot** at resolution, over the arm, and over the frame's bytes too once it is larger than 24 B.
  Completion destroys the frame, steals the continuation head under the node lock, tears down the rest of the arm, constructs the value/error, and publishes `ready` **last**.
  A late subscriber therefore never observes `ready` before the result is in place.
  A large `T` **grows the node naturally**, with no inline cap: `async<int>` / `async<vector<T>>` / `async<string>` are one line, a bigger `T` spills onto further lines.
* Both heads are **one tagged word**: `0` is empty, bit 0 clear is a single inline entry in the high bits, bit 0 set is a slab-backed spill list (nodes are 64-aligned, so the low bits are free).
  The common single-dependency / single-dependent case therefore pays no allocation.
  The continuation head's entries are weak, and its inline slot holds its one weak count by hand (`weak_ptr::adopt` / `release`).
  A second dependent, and every one-shot completion latch, promotes that entry into the list.
* **The compute frame is stored inline** in the payload tail, which on a one-line node leaves it **24 B** — the wrapper's captured `f` plus its `shared_async` dependency handles.
  The rule of thumb is `sizeof(captures) + 8·(dependency count) <= 24`.
  It is constructed once, invoked, and destroyed **in place**, never moved, so parking is free and an immovable frame works (`make_async_lazy_emplace`).
  Running and destroying it are two more `async_type_ops` entries, keyed per frame type.
  A closure that does not fit falls back to a boxed one-pointer `cc::unique_function`, which itself always fits.
  Two things about that budget are easy to trip over.
  It is **type-dependent** — a bigger `T` or `E` widens the payload and the frame slot with it, so the *same* closure can box under `async<int>` and stay inline under `async<big_thing>`.
  And its alignment ceiling is **8, not 16**: the payload is 16-aligned at node offset 16, so a frame at payload + 24 sits at absolute offset 40, and an over-aligned closure is boxed.
  Ask `async<T, E>::frame_fits_inline<F>` rather than restating the numbers — a hand-copied budget goes stale silently, and a silent spill is an allocation per task.

The **semantics and the public API are the contract**.
The node layout is not, and can change under the hood as the system matures without breaking callers.

## Ambient context

One opaque word rides the graph, so code anywhere inside a frame can ask **"which logical task is this work part of?"**.
Two consumers want that.
A test runner attributes a `CHECK` to the right test when tests run concurrently, and a profiler attributes pool time to logical scopes rather than to whichever worker happened to run a node.
cc never learns what is in the word — composition is the tag chain in [`async_ambient.hh`](../../src/clean-core/thread/async_ambient.hh), which is where the API lives.

```cpp
CC_ASYNC_AMBIENT_TAG(my_tag)                          // address-unique, per consumer

cc::async_ambient_scope const s(my_tag(), &my_state); // push; RAII, LIFO
auto* const v = cc::async_ambient_lookup(my_tag());   // from anywhere inside a frame
```

**Attribution is drive-site.**

> A subtree driven from one scheduler work item is billed entirely to that item's context.
> A node inline-driven inside another node's poll is billed to that node's context, transitively.

A node stores the context only as a **resume token**, so a suspended computation resumes under the context it suspended in.
The token is written **once**, by a thread executing or creating the node — never by one merely waking it.
That exclusion is load-bearing rather than an optimization.
Without it a shared dependency completing under B would stamp B onto A's private continuation, and propagate through A's whole remaining subgraph.

There are exactly three write sites, all inside the per-node spinlock those paths already take:

| Transition | Where |
|---|---|
| **cold** → scheduled (never `blocked` → scheduled, which is a wake) | `schedule()`, `schedule_on()` |
| running → **blocked** (park) | `poll()`'s park point |
| running → scheduled (yield) | `reschedule_self()` |

The yield site is the easy one to forget: an inline-driven node was never scheduled, so it holds no token, and without that write it comes back off the queue with none.

`poll()` installs the token for the whole poll, and **only if the node has one**.
A node without one falls through and inherits its driver's context — which is what makes the eager depth-first drive pay one install for a 512-node chain rather than 512.
Node creation costs nothing at all: no TLS read, no store, so born-ready, manual and lazy-then-inline-driven nodes never touch the word.

**Lifetime is safe, not merely checked.**
Links are refcounted and hold their parent strongly, so retaining a head retains the whole chain and a lookup can never reach a freed link.
The arm holds one reference, released in `~async_unresolved` — one site covering resolve-value, resolve-error and cold/parked teardown, so the count is exactly-once by construction.
A **resolved node therefore carries no context at all**, since the token lives in the arm that resolution destroys.

That is what makes **prewarming legal**: work started under a scope that ends before the work does keeps its context alive rather than dangling.
cc deliberately does not assert on it.
A consumer wanting the stricter rule reads `async_ambient_scope::outstanding()` and decides for itself what a leak means.
A test runner in particular wants a reported failure naming the test, not the abort a cc-side assert would give it.

`cc::async_is_polling()` sits alongside, on the same per-thread block.
It answers whether a **throw from here has somewhere to land**: inside a poll it becomes the node's error, outside one it escapes a worker's thread function and terminates.
That is the question a consumer reporting from arbitrary threads has to answer before it throws, and knowing the ambient context does not answer it.

## Cost

Per-node instruction counts, the ladder that decomposes them, and the pinned disassembly probes behind them live in [benchmarks/async-benchmark](../benchmarks/async-benchmark.md).
The headline, on an i9-12900H: `make_async_manual<int>` created and dropped is **128 instructions, zero locked RMW**.
A fully driven `make_async_lazy<i64>` leaf is **509 instructions, 8 atomics**.
The raw slab alloc + free of the same node class is ~3 ns.
These are a snapshot of where the time goes, not a contract — re-measure before quoting one as fact.

For 64 B of storage plus an alloc *and* a dealloc, that is not alarming.

Three properties of the shape constrain what can be changed:

* **Only the refcount *read* is fused, not the accounting.**
  Strong owners hold **one collective weak count**, and `inc` / `release` each touch a single half.
  Giving every strong owner its own weak count — so one RMW could drop both halves — is **rejected**.
  It releases the weak count *before* `destroy_object` runs, so a racing `weak_ptr` drop can free the storage while teardown is still running user destructors.
  The last strong dropper must release the collective weak **after** `destroy_object` returns, which is why `release_strong` never reports `free` on that path.
  The white-box `fused_refcount` tests pin this.
* **`try_lock_strong` CASes the fused word**, so concurrent *weak* traffic can spuriously fail its loop where it could not before fusing.
  That is the one place fusing genuinely charges something.
* **A non-sole-owner strong drop pays the fast-path load for nothing**, and its `fetch_sub` then needs the line Exclusive after that load brought it in Shared.
  The driven leaf has four such drops — `run_one`, `enqueue`'s by-value parameter, the queue element's destroy, `route_after_schedule` — against one that fires.
  All four are the queue round-trip, so driving a root directly instead of `schedule` → enqueue → pop would remove most of them.


**The park path schedules only `cold` deps**, and must keep doing so.
`schedule()` drags a `blocked` node back to `scheduled` and re-enqueues it, so scheduling a dep that is itself parked makes it re-subscribe and re-park.
Down a chain past the inline depth cap that becomes a re-poll storm — measured at 50x worse than doing nothing.

## Not yet here (follow-ups)

* **Structured/owned children** — a parent frame spawning children with borrow-by-reference lifetime.
  Fork/join today uses regular refcounted `shared_async` children, captured and required by the parent frame.
* **Finer routing within one pool** — today every worker serves all work.
  If per-class routing arrives it belongs on the scheduler, not as bytes on every node.
* **Shared errors** and **heterogeneous-`E` propagation** — the failure channel is typed (`async<T, E>`), but the sugar assumes a single `E` across a graph.
  The default `async_error` also still re-materializes its message on propagation rather than sharing an error payload.
  Cross-`E` bridging and cancellation propagation through a graph are follow-ups.
* **Plain (non-async) arguments** in the variadic dependency form.
* **Immovable `T` through `co_return`** — the coroutine layer moves its result through the promise, so an immovable result stays on the raw-frame emplace API.
* **`async<void>`** — you spell `async<cc::unit>` today.
  That is one corner of a wider question about `void` in the vocabulary types, tracked in [TODO](../TODO.md).
* **Deferred teardown** as a built-in — handing a handle to a reclaim thread already works at the user level.
