# systems/recording — `cc::rec`, one event stream under every observability API

Logging, profiling, values, stats and tracing are not five systems here.
They are five vocabularies over one per-thread byte stream, and they differ only in the descriptor a site points at.

That unification is the point rather than a tidiness win.
It makes a **recording a value type**, and once a recording is a value, live observability, crash forensics, CI capture and test assertions become the same mechanism instead of four.

The system is worth proportionally more the less it costs, because annotation that is cheap is annotation that stays in the code.
So the write path is budgeted in instructions, and every convenience that would tax it is pushed to the consumer.

**Nothing is recorded before `cc::rec::initialize()`.**
A library must not decide on the program's behalf how many megabytes it may have, and a binary that never initializes pays only the disabled-site gate.
Test and example binaries get it from `nx::run`, so neither needs a line of setup.

---

## The model

A recording **site** is a `static constexpr cc::rec::desc` plus a write.
The descriptor carries the kind, the level, the name, the source location, the unit and the payload layout — so the stream carries a pointer and the payload bytes, and nothing else.

Being `constexpr` is load-bearing: the descriptor is constant-initialized, so a site has no guard variable, no one-time-initialization check and no first-hit cost.

A **domain** says which part of the source a site belongs to.
It is resolved by ordinary unqualified name lookup, so a site never names one:

```cpp
// in the library's fwd.hh
namespace sg { CC_REC_DECLARE_DOMAIN(g_rec_domain); }

// in exactly one .cc, same namespace
namespace sg { CC_REC_DEFINE_DOMAIN(g_rec_domain, "shaped-graphics"); }
```

Lookup from inside `sg::impl` walks out to `sg`, finds sg's, and stops; a site outside any of them lands on the global fallback.
`domain_fwd.hh` declares nothing but an incomplete type and a `constexpr` address, so a `fwd.hh` pays essentially nothing for it.

**There is no site registry, deliberately.**
The gate indirects through the descriptor to the domain, so reconfiguring one domain is a single word write that reaches every site under it at once.
The crash dumper recovers the descriptor set by scanning the recorded chunks, which is exactly the set that needs interpreting.

---

## What a site costs

Disabled, a site is one load of the domain's mask and one AND.

Enabled, it is a bounds check, a timestamp, a copy and one release store:

```text
load desc.dom->enabled_mask ; test ; jz skip
load tls.cur ; add ; cmp tls.end ; ja cold
rdtscp
stores ; store tls.cur ; release-store chunk->committed
```

Three details in that sequence are not obvious.

**The timestamp is taken after the bounds check, not before.**
The cold path writes bookkeeping events of its own, so a reading taken ahead of it would sort the triggering event before them.
A thread's stream would then go backwards at every rotation.

**`RDTSCP` rather than `RDTSC`**, for the core it was taken on — about ten cycles, and the usual explanation for a step in otherwise steady timings.
Neither instruction is ordered against surrounding code on both sides, so **timestamps within a thread are non-decreasing only after clamping**.
Two readings around a very short span can come back inverted, and a consumer computing a duration must take the max with zero.

**The write cursor is a trivially-destructible, constant-initialized POD.**
A `thread_local` with a non-trivial destructor emits a one-time-initialization guard check on *every* access under MSVC and clang-cl, which would roughly double the cost of a site.
Registration and the thread-exit handshake hang off a separate sentinel installed from the cold path.
A fresh thread starts with `cur == end == nullptr`, so its first record fails the bounds check and registers itself naturally — the call site never carries anything for it.

---

## Chunks

A thread writes into one **chunk** at a time and publishes progress through a single release-stored watermark, which is the only cross-thread word on the write path.
Everything below `committed` is a whole number of complete events, so a consumer reading a live chunk can never see a torn one — the worst it sees is one event of lag.

Chunks come from one pool with a **global byte budget**.
A per-thread budget is the obvious knob and the wrong one: fifty threads times a few megabytes is a gigabyte nobody asked for.
The number the policies actually want to express is a total.
Chunks are large — a megabyte by default — precisely so that acquiring one is rare enough for a plain mutex, and so the per-chunk consumer costs amortize away.

**Chunks are refcounted, and holding a reference IS the capture mechanism.**
A ring buffer of recent activity, a crash dump and a test's recording are all "keep these chunks alive", with no copying anywhere.

A thread's chunks form an SPSC queue consumed strictly in order.
That has one consequence worth stating plainly: **the consumer can only release a chunk once its successor exists**.
So one chunk per recording thread is always retained, and the budget must hold at least two.

Listener output goes into a **separate stream with its own thread state**, not into the ordinary queue.
A half-full listener chunk sitting in the ordinary queue would stall everything written after it, since the queue is consumed in order.

---

## Consumer-written state

Ambient context, the open profiling scopes and the current trace id are stream **state**, not per-event fields.
A producer emits a delta only when one changes, at the three or four sites in `cc::async` that install or adopt an ambient context, and the consumer carries the running value forward.

Each chunk still has to be independently decodable, or ring capture and crash dumps could not start reading in the middle.
**That preamble is written by the consumer, not the producer.**
The actor sees a thread's chunks in order, so it already knows the state at every boundary, and deriving it there costs the producer nothing.
A producer-written preamble would be a variable-size tax on every rotation, for information the consumer would reconstruct anyway.

`chunk_view::state_at_start` is null only for a chunk the consumer never reached, which is the tail of a crash dump.

---

## Policies

| policy | behavior | default in |
|---|---|---|
| `drop` | give up on the event, account it in a gap | release builds |
| `backpressure` | block the producer until a chunk frees up | assertion-enabled builds |
| `grow_unbounded` | ignore the budget | test runs |

Dropping is never silent.
A thread accumulates how many events it lost, over what span and how many bytes, and writes a `gap` event at the head of the next chunk it manages to claim.
So a consumer can always tell "nothing happened" from "we stopped looking".

The cold path is bracketed by two cycle readings and writes a `chunk_acquired` event carrying its own duration, so **the recorder's own overhead is measured rather than modelled**.

A drop storm does not take the pool lock once per lost event: after a failed acquisition the thread waits a configurable interval before asking again.

---

## Listeners and the layer rule

Every listener callback runs under one global processing mutex, so a listener never needs a lock of its own and never sees two callbacks at once.
That holds whether the background consumer or an explicit `flush_blocking()` is driving.

The raw interface is per chunk rather than per event, which is what keeps the consumer cheap: one virtual call per megabyte instead of one per event.
`cc::rec::event_listener<Derived>` is the CRTP adapter for listeners that would rather have per-event dispatch.

A listener may record events of its own — logging from a listener is entirely normal — and the **layer rule** is what stops that from becoming a cycle.

A listener's registration index is its layer, and events recorded from inside listener *i* are offered only to listeners below *i*.
Register the ones that must see everything first.
Entering a layer swaps the write cursor rather than tagging events, because a chunk carries exactly one layer.

**Nested dispatch records nothing.**
A listener may record; a listener reached by another listener's recording may not, and its events are counted as drops.
That is what makes the rule terminate with no cycle detector.

---

## Getting events out

```cpp
cc::rec::initialize();                       // once, from the application

cc::rec::recording_listener capture;
auto const h = cc::rec::register_listener(capture);
do_the_work();
cc::rec::flush_blocking();                   // everything published before this call has been offered
cc::rec::unregister_listener(h);             // blocks until no callback can still be running

auto const captured = capture.take();        // a value: concatenate it, replay it, query it
```

`flush_blocking()` drains on the **calling** thread, under the same processing mutex the background consumer uses.
The guarantee is a happens-before one: every event any thread published before the call has been offered to every listener by the time it returns.

A recording replays into a listener that was never registered, which is what makes one testable offline.

**A recording is process-local**: events point at descriptors, and descriptors are static objects in this binary.

---

## Lifecycle constraints

`initialize()` must be called once; a second call asserts rather than reconfiguring.

`shutdown()` requires that **no other thread is recording**, and that every listener is already unregistered.
It invalidates every registered thread's write cursor before releasing the pool.
A cursor with room left never reaches the cold path, so no amount of checking there could stop a thread from writing into freed memory.
That is also why `thread_state` holds a pointer back into its owner's thread-local cursor.
It is the one thing in the system that reaches across threads that way, and it exists for exactly this.

A generation counter backs the same invariant on the cold path.
Both `initialize()` and `shutdown()` bump it, and a thread whose local copy is stale forgets everything it remembered about the previous incarnation.

Without threads (`SC_THREADS=OFF`, or `config::threaded = false`) no API changes: draining happens on whichever thread flushes, and latency is worse.

---

## What is here today

`cc::rec` currently carries the stream itself: descriptors, domains, the chunk pool, the write path, listeners with the layer rule, the recording value type and replay.

The vocabularies on top of it — `CC_LOG_*`, `CC_RECORD_SCOPE`, `CC_RECORD`, `CC_RECORD_MARK`, `CC_RECORD_STAT` — plus tracing, serialization, the crash dump and the query API are still to come.
`CC_RECORD_EVENT` and `CC_REC_DEFINE_DESC` in [record.hh](../../src/clean-core/record/record.hh) are the seam they all expand into.
The event kinds they need are already reserved in [fwd.hh](../../src/clean-core/record/fwd.hh).

**No binary format here is stable**, and none will be for a good while.
Durability comes from an exporter, not from the raw bytes.
