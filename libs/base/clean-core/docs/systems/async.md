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
  They take their value **by value**, so anything convertible to `T` / `E` works and the conversion happens at
  the call site — the node only ever sees its exact payload type.
* **emplace resolves** — `resolve_to_value_emplace(args...)` / `resolve_to_error_emplace(args...)` build the
  value/error **in place** from `args` (never moved), so an **immovable `T`** works (construct-in-place). The
  by-value `resolve_to_value` requires `T` to be nothrow-move-constructible; the emplace form does not. `args`
  are forwarded by reference into the payload slot, and resolving destroys the frame first (below), so they
  **must not reference the frame's own captures** — nor anything only those captures keep alive, such as a
  dependency's value. The by-value resolves have no such caveat.

A frame is **re-entrant**: one that waits is re-polled once its dependencies are ready. A typical two-phase
frame (register deps → `wait`, then compute) therefore runs twice. Its state persists across polls untouched —
the frame is never moved, so a `mutable` closure just picks up where it left off.

**Resolving is terminal, in the `delete this;` sense.** The frame is stored inline in the node, and the value
is built over the frame's own slot — so a resolve destroys the closure *while it is still running*, then builds
the result there. The rule that makes this safe:

> **A frame must not touch its captures, or the context, after calling a `resolve_*` action.**
> `return actx.success(v);` is the shape — a tail resolve touches nothing afterwards.

The resolve's *arguments* are fine (they are evaluated before the call, and the by-value resolves copy into a
stack temporary first); it is code *after* the resolve that is the hazard. Resolving twice trips an assert.

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
* Each node is heap-owned via `cc::shared_ptr` (8 B, intrusive — see the node layout below), and `schedule()`
  enqueues a `shared_ptr`, so a queued node
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
int v = cc::async_blocking_get_singlethreaded(root);                       // asserts on error/cancel/no-progress
cc::optional<cc::result<int, cc::async_error>> r = cc::try_async_blocking_get_singlethreaded(root); // fallible
```

The `try_` form returns an **optional** result: `nullopt` means the scheduler pumped everything reachable from
here and `root` is still not ready — see "Multi-scheduler correctness" below. `blocking_get` asserts on that
outcome (as well as on an error), so keep it for graphs you know complete inline.

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

**What a published sibling actually costs** (measured, Phase 2): the deque round-trip is nearly free — pushing is
~15 instructions with **no refcount traffic at all**, because a queue entry is a raw pointer whose strong count
is moved in and out by `shared_ptr`'s `release`/`adopt` (a load and a store, no RMW). The real cost is the
**two refcount atomics per published node**, and they are *not* in the deque: they are `route_after_schedule`'s
`from_alive` (`inc_strong`) and the worker dropping the handle after `poll()` (`dec_strong`). That pair is
inherent to a queued entry co-owning its node — a lifetime invariant, not an optimization target — so the way to
remove it is to stop the queue owning the node at all (the same pair is 4 of the 5 strong drops on a driven
leaf; see "The direct path"). That is a node-lifetime redesign, not a scheduler change.

### Multi-scheduler correctness

A node or subgraph **may be reached from more than one scheduler at once** — e.g. an outer API that alternates
single- and multi-threaded computation over asyncs shared with earlier calls, so one subtree is visited by a
`singlethreaded_scheduler` and an `async_thread_pool` concurrently. This is supported and must stay correct. It
is **not optimized for**: performance for a genuinely shared subgraph is not a goal, only correctness.

**Guaranteed under concurrent scheduling.** At most one thread polls a node (`try_begin_running`); a per-node
spinlock serializes state transitions and continuation bookkeeping; a dependency completing while a node runs
records a re-poll instead of enqueuing a second copy; and continuations are held as `weak_ptr`s, so a wake can
never touch a dependent being torn down. No data race, no double-compute — whichever scheduler runs a node, the
result is the same.

**Not guaranteed: which scheduler runs a node.** `has_steal_capable_peers` is read from the *current thread's*
scheduler, so a `singlethreaded_scheduler` driving a subtree inline forces it into single-threaded execution
even where a pool could have parallelized it. And a node can **migrate mid-flight**: when a dependency
completes, `route_after_schedule` sends the woken dependent to whatever scheduler is bound on the **waking**
thread. A graph driven from an st scheduler can therefore finish on a pool (a dependency completed on a pool
worker) or vice versa.

That migration is why `singlethreaded_scheduler::try_blocking_get` returns an **optional**: a drained queue does
not mean the graph is stuck, only that *this* scheduler cannot advance its root — it may be parked on an unpushed
manual node, or have migrated onto another scheduler that is still driving it. `nullopt` is "not from here, not
yet"; push/retry, or let the owning scheduler finish. An st scheduler cannot assume it will ever see every node
of "its" graph, so it reports rather than asserts.

Migration also runs the other way, and it used to strand: a node parked on a pool can be woken on an st thread
(a dependency completed there) and get enqueued onto the st scheduler, which then stops pumping once *its* own
root is ready — leaving the migrated node `scheduled` in a queue nobody drives. Since `schedule()` /
`schedule_on()` are idempotent on `scheduled`, no other scheduler could reclaim it and a `pool.blocking_get` on
it would hang. `try_blocking_get` / `blocking_get` therefore **drain their queue before returning** (with the
worker scope still bound): any migrated-in node is settled — completed, or re-parked as `blocked` and re-woken
later onto whichever scheduler finishes its dependency — rather than left stranded. This runs migrated-in work
single-threaded on the returning thread, the same single-threading a shared subtree already incurs here; the
drain is a no-op in the common case (publish-all-but-one leaves the queue empty once the root is ready).

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
is a **work-stealing** scheduler that runs graphs on real threads. Each worker owns a lock-free **Chase-Lev
deque** ([`impl/chase_lev_deque.hh`](../../src/clean-core/thread/impl/chase_lev_deque.hh)): it pushes and pops
its own bottom end LIFO — freshly spawned children stay hot, and the common path takes no cross-thread sync at
all — while idle workers steal from the top of a *randomly chosen* sibling's deque, which is the only place
threads meet. A shared, mutex-guarded injection queue takes work from foreign threads; it is deliberately not
lock-free, because only genuinely foreign submits reach it (a worker waking a node enqueues locally), so it is
cold by construction.

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

### The hot path costs no shared RMW

The design rule is **no MPMC contention when nobody is actively stealing**, and the thing that matters there is
**coherence traffic, not instruction count**. A `fence(seq_cst)` drains *this core's* store buffer: it touches no
memory and invalidates nothing, so N cores fencing cost O(1) each. A `lock xadd` on a shared counter needs the
line **Exclusive**, so it invalidates every other core's copy and ping-pongs — O(N).

Publishing a node therefore compiles to ~15 instructions with **zero refcount atomics** and exactly one locked
instruction, which is the fence, and which clang lowers to a `lock inc` on a **private stack slot**:

```text
mov  r14, [r8]             ; node.release(): the whole ownership transfer is a load...
mov  qword [r8], 0         ; ...and a store. Count-neutral, no RMW.
mov  [rax+8*rcx+0x10], r14 ; the slot store
mov  [rdi+0x80], rbx       ; publish _bottom (relaxed)
lock inc dword [rbp-0xc]   ; fence(seq_cst) -- private line, no coherence traffic
mov  eax, [rsi+0xc8]       ; _sleepers (relaxed load of a Shared, read-mostly line)
test eax, eax
je   .L1                   ; nobody asleep -> the whole wake path is branched over
```

That the deque holds **raw node pointers** rather than handles is what makes the transfer free: a Chase-Lev slot
is read speculatively by thieves that may lose the race for it, so it cannot hold a smart pointer at all. Each
entry owns one strong count by hand via `cc::shared_ptr`'s `release`/`adopt` pair. **The pool therefore owes
every queued entry a release**, and `~async_thread_pool` drains its deques after joining — without that,
abandoning a 131k-node graph leaks ~49k nodes (pinned by a test).

**Wake protocol.** There is deliberately no counter of claimable work: a worker's scan of the deques already
answers "is there work", so a counter would be a hot-path RMW serving a cold-path question. Instead, a Dekker
store-load cross-pairing, and both sides pay:

* the producer's push ends in a **relaxed** `_bottom` store, so it fences before loading `_sleepers`;
* a would-be sleeper does a seq_cst RMW on `_sleepers`, then **re-scans** before committing to the condvar.

Seq_cst gives one total order over the two, so at least one side sees the other — either the sleeper finds the
work, or the producer sees it and notifies. The producer still passes through the wait mutex on the wake path
(an epoch counter does not remove that: only `wait()`'s atomic release-and-enqueue closes the check-then-wait
window), but that is the wake path only. `_wake_epoch` is the condvar predicate — monotonic, and touched only
when a sleeper exists.

Workers **spin ~64 rounds before sleeping**. Not a micro-optimization: a condvar round-trip is ~1–10 µs against
fork-join tasks that cost ~100 ns, and it is worth ~25% on a spawn-tree (18.2 → 13.6 ns/node). Steal victims are
**randomized** with bounded attempts — a linear scan points every idle worker at worker 0, which is both a
contention hotspot and unfair.

### Measured (i9-12900H, 6P+8E, `relwithdebinfo-clang`)

Scaling is pool-at-P vs pool-at-1-worker, so it is independent of leaf cost. Judge it against the **6 P-cores**,
not the 20 threads: the curve bends at the E-core and SMT boundaries by design.

| case | scaling | note |
|---|---|---|
| reduction | **6.08x** | at/near the machine's ceiling |
| parallel quicksort | **5.42x** | irregular subproblems — the steal-quality case |
| spawn tree (131k nodes, trivial leaves) | **4.97x** | 60.7 → 12.2 ns/node over 1..20 workers |
| nested parallel-for | 4.73x | |
| parallel-for transform | 4.51x | |

The spawn tree is the pure-overhead metric — its leaves do nothing, so its ns/node *is* the pool's cost. The
mutex-guarded predecessor **anti-scaled** on it: 151 ns/node at 1 worker, 1292 at 4 (an 8.5x *slowdown* from
adding three workers). Reproduce with
[pool-benchmark.cc](../../tests/benchmarks/async/pool-benchmark.cc):
`dev.py --mirror-test-output test "bench-async-pool (worker sweep)"`.

Two things that table will not tell you, and that cost real time to learn:

* **This is a laptop, and sustained all-core load throttles it.** The benchmark re-measures its serial baseline
  on *every row* and prints it, precisely so it can be used as a contamination canary: flat = clean, drifting =
  that case's cross-row numbers are not comparable. Absolute ns is only comparable within one run.
* **`vs serial` is an adjacent pair and survives throttling; `vs 1w` spans rows and does not.** Trust the latter
  only where the canary is flat.

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

* **16 B header.** One `atomic<u64>` intrusive refcount — strong in the high half, weak in the low half (the
  handle is one pointer, no separate control block) — plus one `atomic<u64>` control word. Fusing the counts is
  what lets the last strong drop test both with a **single acquire load** and skip both locked RMWs when it is
  the sole owner (`cc::fused_refcount`, see [the direct path](#the-direct-path-measured-cost--optimization-ideas)).
  That word is a **tagged pointer**: a 32-aligned
  `async_type_ops const*` in the high bits, and the lifecycle state + wake-pending flag + spinlock bit in the
  low 5 bits. There is **no C++ vtable** — a static-constexpr `async_type_ops` descriptor is the hand-rolled
  replacement, carrying the typed value/error destructors, the inline frame's invoke/destroy, and the node's
  size class. The descriptor is keyed on what actually distinguishes it — `(size class, value-teardown,
  error-teardown, frame-invoke, frame-destroy)`, not on `(T, E)` — so it **collapses**: a trivially-destructible
  type uses a null teardown, and every *frameless* async (manual/push, born-ready) whose value/error land in the
  same size class with the same teardowns shares one instance — e.g. `async<int, async_error>` and
  `async<float, async_error>` get the same pointer. The frame ops are per frame type, so a framed descriptor is
  one instance per closure — fine, since each closure already emits its own code.
  `is_ready()`/`is_cold()` are lock-free acquire loads of the word.
* **Payload slot (offset 16), one hand-managed union.** The compute frame, the not-ready dependency set, and
  the continuation head (dependents to wake) are **mutually exclusive with** the resolved value ⊍ error — the
  scratch matters only before completion, the value/error only after — so they share the slot, discriminated by
  the node state. The 48 B scratch is **frame 32 + deps 8 + conts 8**. The value is built **straight into the
  slot** at resolution, over the frame: completion destroys the frame, steals the continuation head under the
  node lock, tears down the rest of the scratch, constructs the value/error, and publishes `ready` last — so a
  late subscriber never observes `ready` before the result is in place. The value **grows the node naturally**
  for a large `T` (no inline cap): `async<int>` / `async<vector<T>>` / `async<string>` are one line; a bigger
  `T` spills onto further lines.
* Both heads are **one tagged word**: `0` = empty, bit 0 clear = a single inline entry in the high bits, bit 0
  set = a slab-backed spill list (nodes are 64-aligned, so the low bits are free). The common
  single-dependency / single-dependent case therefore pays no allocation. The continuation head's entries are
  weak, and its inline slot holds its one weak count by hand (`weak_ptr::adopt` / `release`); a 2nd dependent,
  and every one-shot completion latch, promotes that entry into the list.
* **The compute frame is stored inline** in the scratch's 32 B, which fits the closures the sugar builds (the
  wrapper's captured `f` plus its `shared_async` dependency handles). It is constructed once, invoked, and
  destroyed **in place** — never moved, so parking is free and an immovable frame works
  (`make_async_lazy_emplace`). Running and destroying it are two more `async_type_ops` entries, keyed per
  frame type. A closure over 32 B falls back to a boxed one-pointer `cc::unique_function`, which itself fits
  the slot. Cacheline alignment lives on the concrete typed node so unrelated nodes never share a line.

The **semantics and the public API are the contract**; the node layout is not, and can change under the hood as
the system matures without breaking callers.

## The direct path: measured cost & optimization ideas

The floor case — `make_async_manual<int>()` created and dropped, no frame, no scheduler — retires
**128 instructions and no locked RMW at all** (i9-12900H, `relwithdebinfo-clang`); a driven `make_async_lazy<i64>`
leaf, the full create→drive→destroy cycle, retires **509** with 8 atomics. The raw slab alloc+free of the same
node class is ~3 ns, the rest is node overhead. Reproduce the ladder with
[born-ready-benchmark.cc](../../tests/benchmarks/async/born-ready-benchmark.cc) and its pinned probes
(`dev.py assembly trace --target clean-core-test --symbol make_async_manual_probe --stats`); the driven leaf's
probe lives in [async-benchmark.cc](../../tests/benchmarks/async/async-benchmark.cc) under a different test
(`--symbol single_lazy_probe --skip 2 -- "bench-async (single-thread drive)"`).

For 64 B of storage plus an alloc *and* a dealloc this is not alarming. The remaining ideas below are recorded
because the traces show *where* it goes, not because the number is a problem today; they are not committed work.

**A node dies with zero locked RMWs** (done). Teardown used to do `lock dec [node]` (strong) then `lock dec
[node+4]` (weak). The counts are now fused into one `atomic<u64>` (`cc::fused_refcount`, strong high / weak
low), so `release_strong` tests both with a **single acquire load**: reading exactly `(1,1)` proves we hold the
only reference of any kind — no other thread can mint one, because minting requires already holding one — so
there is nobody to race and no RMW is needed. libstdc++'s `_M_release` does exactly this. The manual floor went
**2 atomics → 0** at the same 128 instructions; the driven leaf went 10 → 8.

> **This was on probation, and the probation is over: it stays.** An earlier A/B recorded here said the fast
> path was a single-threaded *wash-to-regression* (empty manual node 4.24 → 3.83 ns, but full born-ready 11.94 →
> 12.57 ns, +5%), and it was kept only on the promise of a payoff against a real work-stealing pool, with a
> pre-committed rule: *re-measure against that pool, and revert if it does not show*.
>
> Both halves were re-measured against the real pool (Phase 2). **The rule does not fire, because its premise
> turned out to be false.** Toggling only the fast path — with it off, `release_strong` always `fetch_sub`s and
> the caller then pays `release_weak`, so 0 RMWs vs 2 — interleaved, on `release-clang`:
>
> | | make_async_manual (empty node) | born-ready full |
> |---|---|---|
> | fast path **on** | **3.58 / 3.59 ns** | **25.3 / 26.0 ns** |
> | fast path **off** | 12.25 / 12.46 ns | 31.2 / 30.4 ns |
>
> Removing it costs **3.4x on the node floor** and ~20% on born-ready. It is not a regression being tolerated on
> a promise; it is a large single-threaded win outright. On the pool the effect is below the noise floor (spawn
> tree at 20 workers: on {12.7, 14.9, 12.9, 13.3, 13.7} vs off {12.5, 12.7, 19.4, 14.7, 12.7}) — which is what
> the old rule asked about, but reverting would trade a measured 3.4x for an unmeasurable one.
>
> **Two caveats, because the old numbers above do not reproduce and that is unexplained.** The old A/B compared
> *fused+fastpath vs pre-fusion separate `u32` counts*; the new one compares *fused+fastpath vs fused, no fast
> path*. Both "before" states do two locked RMWs and ought to cost the same — yet the old pre-fusion floor reads
> 4.24 ns where the fused-2-RMW floor reads 12.25 ns. And the old absolute born-ready figure (11.94 ns) is ~2x
> faster than anything measurable today on the same preset (25–26 ns), so the two runs are probably not measuring
> the same thing at all. Worth a second pair of eyes before trusting either set as a baseline.
>
> One consequence: the recalibration this section used to assert — *"any estimate that prices an uncontended
> atomic at ~20 cycles is suspect"* — does not survive. The 8.9 ns delta for two RMWs on the node floor implies
> roughly that price. The claim it was used to undercut (the case for eliding the per-node spinlock) is therefore
> **open again, not settled**. Price atomics by measurement — but measure them, and re-measure before quoting a
> recorded number as fact.

Three things are worth knowing about the shape:

* **Only the *read* is fused, not the accounting.** Strong owners still hold **one collective weak count**, and
  `inc`/`release` each touch a single half. Giving every strong owner its own weak count (so one RMW drops both
  halves) was considered and **rejected**: it releases the weak count *before* `destroy_object` runs, so a
  racing `weak_ptr` drop can free the storage while teardown is still running user destructors. The last strong
  dropper must release the collective weak **after** `destroy_object` returns, which is why `release_strong`
  never reports `free` on that path. The white-box `fused_refcount` tests pin this.
* **The earlier contention caveat here was over-cautious.** It warned that fusing makes weak and strong traffic
  contend on the same *word* where they previously shared a *line* — but `_strong`/`_weak` were already adjacent
  `atomic<u32>` in one aligned 8 B word of a 64-aligned node, so they already ping-ponged the same line, and
  coherence is per line. `fetch_add`/`fetch_sub` are `lock xadd` on x86: they cannot fail, they serialize, and
  that costs the same on one word as on two words in one line. ARM's LL/SC reservation granule is already ≥ a
  cacheline.
* **The real costs are elsewhere.** (a) Every *non-sole-owner* strong drop pays the fast-path load for nothing,
  and then its `fetch_sub` needs the line Exclusive after the load brought it in Shared — an S→M upgrade the old
  code went straight to. The driven leaf has **four** such drops (`run_one`, `enqueue`'s by-value parameter, the
  queue element's destroy, `route_after_schedule`) against **one** that fires, which is why it rose 488 → 509.
  All four are the queue round-trip; driving a root directly instead of `schedule`→enqueue→pop would delete most
  of them from that path. (b)
  `try_lock_strong` now CASes the fused word, so concurrent *weak* traffic can spuriously fail its loop where it
  could not before. That is the one place fusing genuinely charges something.

**A leaf pays no empty-set teardown** (done). A node with no dependency and no dependent used to walk both sets
out-of-line on every completion and teardown — `unsubscribe_all`, `dep_head::remove_ready`/`clear`, and the
continuation steal + `notify_all` + `~async_cont_head`. Each now sits behind an inline empty-guard with the
scan in an out-of-line slow path, so the leaf skips them entirely: the driven leaf fell **797 → 624**
instructions (nine of the removed direct calls were empty-set walks), the manual floor **167 → 146**. The
guards are safe because a leaf's dep set is poller-owned and its continuation set, checked under the node lock,
is provably empty.

**The manual node births in one store** (done). A promise-style node used to store the ops pointer, then
**reload, mask, and re-store it** to pack `external_pending` into the control word's low bits — two
compile-time constants that would not fold, because `_state_and_ops` is `std::atomic<u64>` and the compiler
will not forward a value across atomic accesses. `init_control_word(ops, state)` writes both in a single
relaxed store (safe: the node is not shared during construction), via a manual-tag constructor `make_async_manual`
selects. Shaved the `make_async_manual` floor 146 → 142 instructions. The cold (lazy/scheduled) ctor never had
the second store — it births `cold`, whose state bits are zero — so it is unchanged.

**The frame is stored inline** (done). `make_async_lazy` used to cost a second node alloc for the closure (a
16 B poly node — no SSO) on top of the 64 B node, and installing that 8 B pointer cost **four** out-of-line
calls (~60 instructions) because `poly_node_allocation::operator=(&&)` and its temporaries never inline;
`~poly_node_allocation` alone was 78 instructions on the driven-leaf trace. The closure now lives in the
scratch's 32 B, reached through two `async_type_ops` entries. The driven leaf fell **624 → 488** instructions
and lost an entire malloc/free pair — a cold `make_async_lazy` is now only **+0.51 ns** over an empty node.
`async_frame_destroy` for a trivial closure is one instruction.

Making room for it meant folding `async_cont_head` from 16 B to one tagged word, so the arm is frame 32 +
deps 8 + conts 8 = 48. Dropping the move also made parking free (the frame is re-entered in place) and let an
immovable frame work at all; the cost is that a resolve now destroys the closure mid-call, which is the
`delete this;` rule documented under the raw compute frame.

**More instructions are still pure overhead** and unambiguously safe to remove:

* A `/GS` stack cookie in `free_storage`, which has no buffers, on the hottest free path.
* `free_storage` chases `node -> ops -> class_index` (a dependent load into a cold cacheline) to fetch a
  constant, then bounds-checks it.

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
* Finer per-worker routing within one pool — today every worker serves all work. (The lock-free per-worker
  deque has landed; see "Concurrent execution".)
* **Shared errors** and **heterogeneous-`E` propagation** — the failure channel is now typed (`async<T, E>`),
  but the default `async_error` still re-materializes its message on propagation (no shared error payload yet),
  and the high-level sugar assumes a single `E` across a graph. Cross-`E` bridging and cancellation propagation
  through a graph are follow-ups.
* `co_await` integration layered on top of the raw frame API, and plain (non-async) arguments in the variadic
  dependency form.
