# cc::async — a value/dataflow async system

`cc::async<T, E = async_error>` is a low-overhead async for **compute-heavy dependency graphs**. The mental
model is values and dataflow transformations, not futures/promises or callback chains. It is the foundation for
the CPU fan-out "task system" that `cc::threaded_actor` defers to. `E` is the failure-channel type; it defaults
to `async_error` (a move-only wrapper over `cc::any_error`), but any type works (an enum, a small struct, …).

Headers: [`clean-core/thread/async.hh`](../../src/clean-core/thread/async.hh) (public, templated) and
[`clean-core/thread/async_node.hh`](../../src/clean-core/thread/async_node.hh) (untemplated core + scheduler
seam). This is **incubator-stage** API — expect it to grow and change.

## Model

An `async<T, E>` is an eventual `result<T, E>` produced by a **compute frame**: a callable / state-machine
polled through an `async_context<T, E>`. Failure and cancellation are **values** (the default `async_error`
carries both), not exceptions or out-of-band control flow.

```cpp
auto a = cc::make_async_scheduled<int>([](cc::async_context<int>&) { return 40; });
auto b = cc::make_async_lazy([](int x) { return x + 2; }, a);   // b depends on a; f gets a plain int
int v = cc::async_blocking_get_singlethreaded(b);   // drives the graph on this thread -> 42
```

The handle:

* **`shared_async<T, E>` = `cc::shared_ptr<async<T, E>>`** — the normal, composable handle (an 8 B intrusive
  refcount handle over one slab node). Many dependents may observe it. `async<T, E>` itself is non-copyable and
  immovable; you copy the handle, never the node.

### The raw compute frame

A frame is a callable `async_step_status(async_context<T, E>&)`: it resolves its outcome **through** the typed
context and returns a status. It may be a hand-written state machine that adds dependencies dynamically as it
runs:

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

A raw frame's return carries no value type (it returns a status), so its node must give `T` explicitly —
`make_async_lazy<int>(...)`. A plain value-returning frame (`[](int x){ return x + 1; }`) deduces `T`
context-free — a value frame that *also* takes a context must give `T` explicitly.

`async_context<T, E>` gives a frame:

* `require(dep) -> bool` — true if `dep` is already ready (read its value now); otherwise records it as a
  pending dependency and returns false. **It neither subscribes nor schedules** — the poll loop owns both (see
  publish all-but-one). `dep` may be a `shared_async` created earlier or one the frame builds on the fly
  (dynamic dependencies) — capture it so it stays alive.
* resolve the result — each returns the matching status, so `return actx.xxx(...)`: `resolve_to_value(v)` /
  `resolve_to_error(E)` (aliased `success(v)` / `error(...)`; for the default `E`, `error(any_error)` wraps),
  plus `wait_for_dependencies()` and `yield()`. Both resolves complete the node **in place** as the frame runs.
* **emplace resolves** — `resolve_to_value_emplace(args...)` / `resolve_to_error_emplace(args...)` build the
  value/error **in place** from `args` (never moved), so an **immovable `T`** works (construct-in-place). The
  by-value `resolve_to_value` requires `T` to be nothrow-move-constructible; the emplace form does not.

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
Its loop: drop ready deps; if any remain, **drive one inline, depth-first** — the poller descends into a
not-ready dependency's own `poll()` on the current stack (a cold node polls fine, which is what lets `require()`
stay out of scheduling) and re-evaluates. Only when a dependency cannot be completed inline — a manual/push
node, one already running on another worker, or the depth cap — does it fall back to scheduling the remaining
deps, installing wakeup continuations, and parking. Otherwise it runs a compute step, and on completion
publishes the result and wakes dependents. It never blocks a thread.

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

Two layers, two contracts:

* **The high-level `make_async_*` sugar auto-propagates.** If a dependency completed with an error, the
  dependent async (a `map` or a variadic dependency form) completes with that error and `f` never runs. The
  sugar assumes a **single failure type `E`** across the graph — a dependency's propagated error must be
  constructible into the dependent's `E`. `async_error` also carries cancellation.
* **A raw compute frame does NOT auto-propagate — the frame decides.** A dependency that resolved to an error
  still counts as *ready*, so the frame is re-run; it must check `dep->try_error()` itself and choose to
  propagate, transform, or ignore it:

  ```cpp
  if (!ctx.require(dep)) return ctx.wait_for_dependencies();
  if (auto const* e = dep->try_error()) return ctx.resolve_to_error(/* map *e to this node's E */);
  return ctx.resolve_to_value(*dep->try_value());
  ```

**Propagation strategy (`impl::async_error_propagate`).** Copying an error out of a shared node uses a per-`E`
hook: a copyable custom `E` is **copied**; the default move-only `async_error` is **re-materialized** from its
message (the context chain is not preserved — a richer error-sharing scheme is a follow-up), since a shared
node's `any_error` must not be moved out. Cross-node propagation across a **heterogeneous-`E`** graph is not
wired into the sugar yet; bridge it by hand in a raw frame.

## Driving (the scheduler seam)

**You never block on an async — a scheduler makes progress on it**, and blocking is a convenience that
scheduler offers. The graph itself is **decoupled from any executor**: a worker binds a scheduler to its thread
with `async_worker_scope`, and nodes reach it via `async_scheduler::current()`.

There are two schedulers, and they present the same surface:

| | drives | publishes work | use |
|---|---|---|---|
| `singlethreaded_scheduler` | inline, on the calling thread | never | tests, debug, deterministic runs |
| `async_thread_pool` | worker threads | yes (unless 1 worker) | real concurrent work |

```cpp
cc::singlethreaded_scheduler sched;
int v = sched.blocking_get(root);        // drives + blocks THIS thread
cc::async_thread_pool pool(cc::num_hardware_threads());
int v = pool.blocking_get(root);         // submits + blocks the (foreign) calling thread
```

`singlethreaded_scheduler` is single-threaded **by construction, not by circumstance**: it has no peers, so it
never publishes work and a graph's nodes cannot run concurrently however many cores sit idle. That is what
makes the whole system testable without threads.

For a self-contained graph, the free functions build a throwaway one for you. The verbose names are
deliberate — this is a test/debug convenience, not how real work gets scheduled:

```cpp
int v = cc::async_blocking_get_singlethreaded(root);                       // asserts on error/cancel
cc::result<int, cc::async_error> r = cc::try_async_blocking_get_singlethreaded(root); // fallible
```

`run_one` / `run_until` / `drain` are the underlying pump, and the pump is what you need when a graph parks on
a manual node: nothing progresses while you are not inside it, so call it again after the external push.

```cpp
cc::singlethreaded_scheduler sched;
cc::async_worker_scope scope(sched);
root->schedule();
sched.run_until([&] { return root->is_ready(); }); // interleave an external push here, then pump again
```

### Publish all-but-one

A node's poll loop drives one dependency inline on its own stack, so enqueuing *that* one would be pure churn:
it would be popped later as a ready no-op, and until then the queue's strong handle would pin it alive. The
loop therefore publishes only the **other** dependencies, and only when
`async_scheduler::has_steal_capable_peers` says someone could actually claim them. `require()` neither
schedules nor subscribes — the poll loop owns both, and schedules whatever is left before it parks.

For a `singlethreaded_scheduler` this means chains and single-dependency transforms enqueue **nothing at all**;
for a pool, a fan-out of n publishes n−1 stealable siblings. A 1-worker pool reports no peers, so it behaves
like the single-threaded case.

This is a lifetime property as much as a perf one: a queued entry is a strong node handle, so work abandoned in
a queue pins its graph alive. It is pinned by the "reused scheduler settles empty" test.

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

`try_value()` / `try_error()` return non-owning pointers **into** the node's payload — copy-free and stable
while the node is alive (you keep it alive through the handle).

To **move** the outcome out instead of reading it in place, consume the handle:

```cpp
cc::result<int, cc::async_error> r = cc::into_result(cc::move(a));  // a must be ready; MOVES value/error out
```

`into_result` takes the handle by value and moves the payload out into a `cc::result<T, E>`. Because it moves
out of shared node storage, **any other live handle's later `try_value()`/`try_error()` reads a moved-from
value** — use it when you are done with the async. `T` must be move-constructible (a truly immovable `T` cannot
be `into_result`'d — that is a compile error by design; read it in place via `try_value()`).

### Born-ready factories

For a value/error known up front, skip the frame and scheduling entirely:

```cpp
auto rv = cc::make_async_from_value(42);                          // ready_value, drivable as a dependency
auto re = cc::make_async_from_error<int>(async_error::make_cancelled());
auto ri = cc::make_async_from_value_emplace<Immovable>(7);        // build T in place (immovable T ok)
// also make_async_from_error_emplace<T, E>(args...)
```

## Concurrent execution: `async_thread_pool`

`cc::async_thread_pool` ([`clean-core/thread/async_thread_pool.hh`](../../src/clean-core/thread/async_thread_pool.hh))
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

### Node layout (size & locking)

A node is a **16 B header** followed by a payload slot; `async<int>` is **64 B — one cache line** (down from an
original 384 B), and the node is cacheline-aligned to avoid false sharing between unrelated nodes.

* **16 B header.** Two `atomic<u32>` intrusive refcounts (strong + weak — the handle is one pointer, no
  separate control block) plus one `atomic<u64>` control word. That word is a **tagged pointer**: a 32-aligned
  `async_type_ops const*` in the high bits, and the lifecycle state + wake-pending flag + spinlock bit in the
  low 5 bits. There is **no C++ vtable** — a static-constexpr `async_type_ops` descriptor is the hand-rolled
  replacement, carrying the typed value/error destructors and the node's size class; the frame is invoked
  directly. The descriptor is keyed on `(size class, value-teardown, error-teardown)`, not on `(T, E)`, so it
  **collapses**: a trivially-destructible type uses a null teardown, and every async whose value/error land in
  the same size class with the same teardowns shares one descriptor instance — e.g. `async<int, async_error>` and
  `async<float, async_error>` get the same pointer. `is_ready()`/`is_cold()` are lock-free acquire loads of the word.
* **Payload slot (offset 16), one hand-managed union.** The compute frame, the not-ready dependency set, and
  the continuation head (dependents to wake) are **mutually exclusive with** the resolved value ⊍ error — the
  scratch matters only before completion, the value/error only after — so they share the slot, discriminated by
  the node state. The value is built **straight into the slot** at resolution: the poll loop moves the frame
  onto its own stack for the compute step (so the value can overwrite its slot; the frame is moved back if it
  parks), and completion steals the continuation head under the node lock, tears down the scratch, constructs
  the value/error, and publishes `ready` last — so a late subscriber never observes `ready` before the result
  is in place. The value **grows the node naturally** for a large `T` (no inline cap): `async<int>` /
  `async<vector<T>>` / `async<string>` are one line; a bigger `T` spills onto further lines.
* The continuation head keeps **one inline weak dependent** (the common single-dependent case pays no
  allocation) and spills the rest, plus any one-shot completion latch, into slab-backed cells.
* The compute frame is a **one-pointer `unique_function`** (closure + type-erased ops in one node; see
  `cc::poly_node_allocation`). Cacheline alignment lives on the concrete typed node so unrelated nodes never
  share a line.

The **semantics and the public API are the contract**; the node layout is not, and can change under the hood as
the system matures without breaking callers.

## The direct path: measured cost & optimization ideas

The floor case — `make_async_manual<int>()` created and dropped, no frame, no scheduler — costs **~17 ns /
~80 cycles** and retires **167 instructions** (i9-12900H, `relwithdebinfo-clang`). The raw slab alloc+free of
the same node class is ~2 ns, so ~15 ns is node overhead. Reproduce the ladder with
[born-ready-benchmark.cc](../../tests/benchmarks/async/born-ready-benchmark.cc) and the instruction counts by
tracing its pinned probes (`dev.py assembly trace --target clean-core-test --symbol make_async_manual_probe`).

For 64 B of storage plus an alloc *and* a dealloc, ~80 cycles is not alarming. The ideas below are recorded
because the traces show *where* it goes, not because the number is a problem today. None is committed work.

**The two `lock dec` are over half the budget.** Teardown does `lock dec [node]` (strong) then `lock dec
[node+4]` (weak) — ~20 cycles each uncontended. Since `_strong`/`_weak` are adjacent `atomic<u32>` in one
aligned 8 B word, a single 64-bit load could check for the sole-owner `(1,1)` case and skip both locked RMWs
(libstdc++'s `_M_release` does exactly this). **Caveat:** any scheme that fuses the two counters into one word
makes weak and strong traffic contend on the same cacheline word where they previously only shared a line —
fine if weak refs stay rare (continuations hold them), potentially worse if they don't. Needs measuring, not
assuming.

**~45 instructions are pure overhead** and are unambiguously safe to remove:

* Three trivially-inlinable header functions that clang left out-of-line in `teardown_payload`
  (`async_dep_head::for_each`, `~async_dep_head`, `~poly_node_allocation`) — collectively ~31 instructions and
  three stack frames to test three pointers against zero.
* The node ctor stores the ops pointer, then **reloads, masks, and re-stores it** to pack the initial state
  into its low bits. Both are compile-time constants, so this should be one store — it doesn't fold because
  `_state_and_ops` is `std::atomic<u64>` and the compiler won't forward across atomic accesses. During
  construction the node isn't published yet, so the atomic is unnecessary there.
* A `/GS` stack cookie in `free_storage`, which has no buffers, on the hottest free path.
* `free_storage` chases `node -> ops -> class_index` (a dependent load into a cold cacheline) to fetch a
  constant, then bounds-checks it.

**The lazy path allocates twice.** `make_async_lazy` costs a second node alloc for the closure (16 B poly node
— no SSO) on top of the 64 B node, and installing that 8 B pointer into the frame slot costs **four**
out-of-line calls (~60 instructions) because `poly_node_allocation::operator=(&&)` and its temporaries never
inline. Small-closure SSO in the node's payload would remove the second alloc entirely.

**Deferring teardown** to another thread is possible today at the user level (hand the handle to a reclaim
thread) and is not planned as a built-in.

### What publish-all-but-one was worth

Before it, `require()` scheduled every cold dependency and the eager drive then completed it inline anyway,
leaving a ready no-op in the queue that `run_until` never popped. A reused scheduler's live-node set grew
without bound, the slab stopped recycling hot 64 B slots, and the working set thrashed the caches. Measured on
the drive benchmark ([async-benchmark.cc](../../tests/benchmarks/async/async-benchmark.cc), i9-12900H):

| case | before | after |
|---|---|---|
| single-dep a→b | 1732 ns/op | 81 |
| chain N=64 | 6352 ns/op | 69 |
| chain N=512 (past the depth cap) | 8561 ns/op | 154 |
| fan-in c=f(a,b) | 6306 ns/op | 80 |
| sum-tree depth 13 | 12329 ns/op | 84 |

`born-ready` (~31 ns) and `single lazy` (~90 ns) were never affected — they have no dependencies, so they
retained nothing.

One subtlety worth keeping: the park path schedules only **cold** deps. `schedule()` drags a `blocked` node
back to `scheduled` and re-enqueues it, so scheduling a dep that is itself parked makes it re-subscribe and
re-park — down a chain past the inline depth cap that becomes a re-poll storm (measured: 21672 ns/op on
`chain N=512`, 50x worse than doing nothing).

## Not yet here (follow-ups)

* **Structured/owned children** — a parent frame spawning children with borrow-by-reference lifetime. An
  earlier `spawn_child` / `await` subsystem was removed pending a real use case; fork/join today uses regular
  refcounted `shared_async` children captured and required by the parent frame.
* A **lock-free** per-worker deque (Chase-Lev) and finer per-worker routing within one pool; today the deques
  are mutex-guarded and every worker serves all work.
* **Shared errors** and **heterogeneous-`E` propagation** — the failure channel is now typed (`async<T, E>`),
  but the default `async_error` still re-materializes its message on propagation (no shared error payload yet),
  and the high-level sugar assumes a single `E` across a graph. Cross-`E` bridging and cancellation propagation
  through a graph are follow-ups.
* `co_await` integration layered on top of the raw frame API, and plain (non-async) arguments in the variadic
  dependency form.
