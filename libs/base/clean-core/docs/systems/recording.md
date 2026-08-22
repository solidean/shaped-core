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

**A thread's timestamps STRICTLY increase**, which is a guarantee the recorder makes rather than one the hardware gives.
Every reading goes through a per-thread monotonic stamp — `max(reading, last + 1)` — so neither a tie nor an inversion is observable.

Both failure modes are real without it.
The counter is not ordered against surrounding code on both sides, so two readings around a very short span can come back inverted.
And a tick is coarse enough on some platforms that a tight loop ties outright: about 42 ns on Apple silicon, where several events land inside one.

A tied stream costs real information rather than precision.
A scope that opens and closes within a tick has no duration, a scope pair tying with the pair it encloses cannot be nested, and `in_cycle_range` between two adjacent events is empty.

**The cost is that time is manufactured when events outrun the clock.**
A burst arriving faster than the counter ticks gets one tick per event whatever it really took, so a scope wrapping many of them reads long by up to one tick each.
That is proportional to the platform's resolution, self-correcting at the first gap, and only reached where the recorder's own cost already dominates what is being measured.

Across threads there is no such guarantee: two threads can stamp the same tick, and a query that orders across them breaks the tie by capture order.

**The write cursor is a trivially-destructible, constant-initialized POD.**
A `thread_local` with a non-trivial destructor emits a one-time-initialization guard check on *every* access under MSVC and clang-cl, which would roughly double the cost of a site.
Registration and the thread-exit handshake hang off a separate sentinel installed from the cold path.
A fresh thread starts with `cur == end == nullptr`, so its first record fails the bounds check and registers itself naturally — the call site never carries anything for it.

---

## The vocabularies

Every one of these is the same descriptor-plus-write underneath, and they differ only in kind, category and payload.
`clean-core/common/log.hh` and `clean-core/common/profiling.hh` are the convenience includes; nobody has to know the folder is called `record/`.

**A site's name must be a compile-time constant.**
It lives in the site's `static constexpr` descriptor, which is what keeps a site free of a guard variable — so a helper taking a runtime `char const*` does not compile, by construction.

### Logging

```cpp
CC_LOG_INFO("shader cache warmed");            // no payload at all: the text is the descriptor
CC_LOG_WARNING("fell back to {} after {}", name, reason);
```

A message with no arguments costs the stream nothing beyond its header.
One with arguments is formatted directly into the chunk's remaining space by `cc::format_to`, so there is no temporary buffer, no allocation and no copy.

The format string doubles as the site's name, so every message from one site groups under one string whatever it formatted to.
That is what makes "how often does this fire" answerable at all.

A message too long for what is left of a chunk is **truncated and flagged, never dropped** — a truncated message is still evidence.

Levels are `trace`, `debug`, `info`, `warning`, `error`, and each gates on its own bit in the domain's mask.
`trace` and `debug` are off by default, because a build that records them by default teaches everyone to turn logging off.
A domain can also be told to capture a stack or break into the debugger at a level; errors capture a stack by default.

### Profiling scopes

```cpp
CC_RECORD_SCOPE();                 // named after the enclosing function
CC_RECORD_SCOPE("upload-pass");    // named explicitly
CC_RECORD_SCOPE_BEGIN("span"); ... CC_RECORD_SCOPE_END("span");   // when the ends are in different functions
```

A scope opens and closes on one thread at one nesting depth, and both events carry that depth.
Four bytes of payload buys best-effort re-nesting of a stream that lost its middle, which is exactly the stream a crash dump or a decimated ring buffer hands you.

**A scope must not cross a `co_await`.**
The work stopped, the thread went elsewhere, and the span it would report never happened.
`cc::async`'s frame driver has exactly one place where a coroutine body runs, and it asserts there that the body left the scope depth where it found it.
So the mistake is caught rather than reported as a wrong number.

### Async scopes

```cpp
CC_RECORD_ASYNC_SCOPE("load-level");                       // follows the work, wherever it resumes
CC_RECORD_ASYNC_SCOPE_WITH_ID("inbound", id_off_the_wire); // ... under an id from somewhere else
auto const* s = cc::rec::current_async_scope();            // the innermost one, or null
auto const id = cc::rec::current_trace_id();               // ... and the trace it is
```

An async scope is the answer to the restriction above: it is an entry in `cc::async`'s ambient chain rather than a stack frame.
So a `co_await` carries it along, and every task spawned underneath inherits it.
That makes it the right tool for a logical operation — a request, a job, a level load — and `CC_RECORD_SCOPE` the right one for a span of one thread's time.

It is also strictly the more expensive of the two.
Installing one allocates two links and takes their refcounts, where a profiling scope writes two events and touches a counter.
Per operation that is nothing; per inner-loop iteration it is the wrong tool.

**An async scope IS a trace, and its id is not optional.**
The id is what the stream attributes by, so a scope without one would propagate a context no recording could name.
Every scope therefore mints one unless handed one, and `cc::rec::current_trace_id()` reads it back off the chain — which is why a trace now follows a `co_await` where a thread-local one could not.

The scope writes an `async_scope_begin` / `async_scope_end` pair carrying that id.
That pair is what puts a **name** on a trace: an id is minted at runtime and a descriptor is static, so nothing else in the stream could say that trace `0x0800…03` was called `handle-request`.
The pair brackets the scope OBJECT's life, not the work under it — that outlives the scope by design.

**The deltas are eager, and that is the whole design.**
Whenever `cc::async` restores a context whose trace differs from the one this thread last published, the recorder writes an `ambient_changed` event naming the new one.
A lazy scheme — stamping the ambient onto the next event that happens to be recorded — would be free on the write path and wrong.
A chain of `co_await`s that logs nothing would be billed to whatever context preceded it, and an async scope exists precisely to show where **time** goes.

The cost at each restore site is a short chain walk for the id plus one compare, and a node carrying no ambient token never reaches even that.
A worker draining related items restores the same context repeatedly, and those repeats stop at the compare.

**The delta carries the ID rather than the ambient address**, which is what makes it free.
An address is unique only while its link lives, so an earlier version pinned each head into the chunk to reserve it.
That cost one pin per context switch against a 64-slot array, force-rotating a whole megabyte chunk every 64 switches.
That measured as a 75% tax on an async-heavy workload, bought for an identity a value carries for nothing.
A recording therefore outlives every link it ever saw and still tells the contexts apart.

### Sampling

```cpp
#include <clean-core/record/sampling.hh>
cc::rec::sampling_scope const sampling({.rate_hz = 1000.0});   // or start_sampling / stop_sampling
...
auto const merged = captured.spliced_samples();
```

Instrumentation says how long the things you NAMED took, and is blind to everything nobody named.
A sampler answers the other half, and the two compose rather than compete.

**A sample is written to the sampler's own stream, never the sampled thread's.**
A suspended thread may be mid-event, or mid-rotation holding the pool lock, so writing into its stream from outside would corrupt it or deadlock against it.
Each sample instead carries an **anchor** — which thread it caught, and how far that thread's stream had committed — and `recording::spliced_samples()` puts it back there.

The anchor is a POSITION rather than a copy of the state at that position, and that is what makes it worth more than a trace id would be.
A reader replaying the anchored thread in order reaches the sample with the trace and the open scope stack already in hand — from that chunk's preamble, and from the scope events since.
The scope stack is the half that matters: a sample stops at `current_scope_frame()`, and the frames below it are exactly what that stack names.

So a sample taken inside instrumented code is **short on purpose** — often a single address — and splicing supplies the rest.
That cuts the walk and the payload by the same factor, which matters most for sampling, the one thing that can flood the stream.

The order the sampler obeys is not negotiable: take the registry lock, suspend, read the anchor and walk into a fixed buffer, resume, and only then write.
A suspended thread may hold the allocator's lock or the pool's, so allocating or writing before the resume hangs on a lock its owner cannot release.

**The sampler records its own ticks**, as `record.sample_tick` scopes on its own lane.
A sampled profile is only as trustworthy as its cadence, and a reader asking whether a 16 ms frame was sampled evenly or aliased against it needs to see when the ticks landed and what each one cost.

`rate_hz` is a per-thread rate, because a tick covers every thread by default.
It is capped by what the OS timer delivers, which measured about **1.9 kHz** on Windows.
A high-resolution waitable timer floors near half a millisecond, and neither `timeBeginPeriod` nor a periodic timer beats that.
`threads_per_tick = 1` gives a fixed budget split across however many threads exist instead.
For one frame's worth of ticks that is a couple of samples each, which is not a profile of anything.

Two limits worth knowing.
Only threads the recorder already knows are sampled, which a thread joins by recording anything at all — enumerating the process's OS threads instead is in the [TODO](../TODO.md).
And there is no sampler at all without threads (`SC_THREADS=OFF`) or on a platform that cannot walk a foreign thread's stack; `is_sampling()` reports which you are in rather than pretending.

### Values, markers and stats

```cpp
CC_RECORD_MARK("fallback-taken");                          // did this code run
CC_RECORD("mesh_vertices", vertex_count);                  // with what
CC_RECORD_STAT("queue_depth", cc::rec::unit_count, n);     // the current reading
CC_RECORD_ACCUM("bytes_uploaded", cc::rec::unit_bytes, n); // a delta to add up
```

A marker is the cheapest useful annotation there is, and the one to reach for in a fallback branch you are not sure is ever taken.

`CC_RECORD` takes scalars, enums, pointers and text.
An enum collapses onto its underlying type and a pointer onto an opaque address, so a consumer reads a number without knowing the type.

Text has two tiers, and **the const is what picks between them**:

| written as | stored as | why |
|---|---|---|
| `char const[N]` — a string literal | its address (`type_code::cstring`) | the bytes are in the binary and outlive everything |
| `char[N]`, `char const*`, `cc::string_view`, `cc::string` | its bytes (`type_code::inline_text`) | nobody promised to keep the memory |

The split lands where it should without anyone thinking about it: a buffer you formatted into has to be non-const to have been written, so it is copied.
A reader sees no difference — `first_text` and `field_as_text` return the same string either way — so the tier is an encoding decision and never one a caller has to track.

**A literal's bytes must outlive the process**, because the event carries only their address.
Nothing reads the payload at the recording site: the actor drains the chunk later, a capture holds it for as long as it lives, and serializing walks it later still.
So a `char const buf[32]` local is a garbage read at every one of those points, whatever it was NUL-terminated with — pass `cc::string_view(buf)` to copy it instead.
That case is misuse rather than something the type system catches, which is the price of the split.

A `cstring` is the second payload field that holds a pointer into this binary.
So serializing routes it through the string table and loading gives it an arena of its own — the same treatment `desc_ref` gets, for the same reason.

### Recording bytes by pin

`CC_RECORD_PINNED("frame.depth", pinned)` writes an address and a size, and hands the chunk a reference that keeps the bytes alive.
So a megabyte of mesh or framebuffer data costs the stream sixteen bytes, and a test or a debugger gets the real thing rather than a summary somebody remembered to write.

**Opt-in per type, through `cc::rec::pinnable_traits`.**
Pinning is a promise about lifetime and immutability that only a type's author can make, so nothing is pinnable by accident.
`cc::pinned_data<T>` is pinnable out of the box, which is the whole point of that type.

**The bytes must not change while any recording holding them is alive.**
Nothing copies them, so a mutation after the fact rewrites history instead of being recorded as a second value.

A chunk's pin array holds 64, and that is not a limit.
A full array moves into a heap block, the block becomes a single pin, and 63 slots come free — repeatable without bound, which is why releasing one walks a chain rather than a flat list.
`try_add_pin` therefore allocates, and is only ever reached from a site that asked to pin something.

Serializing copies the pinned bytes into a **blob section**, last in the file because it is the only section with no bound on its size.
The payload's address becomes an offset into it, and loading turns that back into a pointer at the loaded recording's own copy.
Read one through `event_view::field_as_bytes`, which reads the same on a live recording and a loaded one.

`CC_RECORD_NAMED(name, value)` is the form for a name only known until runtime, and it is the exception rather than the tool.
A static name costs the stream nothing because it lives in the descriptor; a runtime one is copied into every event, and a query keyed on it reads the payload rather than comparing a pointer.
Its value must be fixed-size — a scalar, an enum, a pointer or a literal — because a field's offset is a constant, so at most one variable-length field can exist and it has to be last.
`event_view::name()` returns whichever the site has, so every name-keyed query works on both without knowing which it got.

The two stat kinds are not interchangeable, and picking the wrong one produces a plausible graph of the wrong thing.
A snapshot is the current reading of something that exists whether or not you look; summing snapshots is meaningless.
An accumulate is a delta, and summing is the whole point.

Values are `f64` only, which also covers every integer up to 2^53.
That is a deliberate cap: one numeric type means a listener graphs anything without a type switch.

A `cc::rec::unit` says what a quantity means — singular and plural, symbol, prefix base, axis scale, aggregation, preferred range, whether higher is better.
Deliberately a struct rather than an enum: everyone's enum of units is missing the case the next consumer needs.

### Tracing

A profiling scope answers "what was this thread doing", and it nests because a call stack does.
Tracing answers the harder question: **which of these events belong to the same logical operation**.
That operation spans threads, queues, retries and caches, and its parts have no lexical relationship at all.

The whole mechanism is two things — an id that costs nothing to mint, and an event saying that some ids are related.

```cpp
CC_TRACE_SCOPE("handle-request");                  // mints and names the trace
auto const id = cc::rec::current_trace_id();
CC_TRACE_SCOPE_WITH_ID("inbound", wire_id);        // ... or carry one from off the wire

CC_RECORD_RELATION(cc::rec::relation_parent_of, request, fetch);
CC_RECORD_RELATION(cc::rec::relation_same_key_as, a, b, c);   // n-ary
CC_RECORD_RELATION_MANY(type, discovered_members);            // a runtime member list
```

**Naming the trace is the point.**
A bare id is an opaque number, and a viewer showing `0x0800000000000003` helps nobody, so a scope names the trace and mints its id.

**A relation type is a static object, not an enum**, for the same reason a `cc::rec::unit` is: an enum of relation kinds is always missing the case the next consumer needs.

```cpp
struct relation_type { char const* name, * inverse_name; bool is_symmetric, is_transitive, is_equivalence; };
```

`inverse_name` is what lets a viewer render an edge from either end without hardcoding a vocabulary.
`is_equivalence` is the one flag a reconstruction can act on directly: the members may be **merged into one logical operation**.
That is exactly the "these turned out to be the same work" case, and `cc::disjoint_set` is sitting right there.
The built-ins are `relation_parent_of`, `relation_caused_by`, `relation_same_key_as` and `relation_follows`; define your own next to the code that records it.

**Relations are n-ary**, because several genuinely are: five requests that hit one cache key, eight inputs to one join.
Decomposing those into pairs against a representative loses the fact that they were related *as a group*.
The convention is **first member is the subject, the rest are objects**, which covers a fan-out (`parent_of(parent, children…)`) and a fan-in (`caused_by(effect, causes…)`) with one rule.
Order carries nothing for a symmetric type.

**The graph is reconstructed entirely offline.** Nothing in the recorder builds one.
That is what makes a *late* discovery free.
When a computation turns out to have produced a cache key another operation already used, you record `same_key_as` the moment you learn it, and the reconstruction does not care that it arrived last.
An id that nothing tracks also cannot leak, cannot be looked up wrongly, and costs nothing to abandon.

Trace membership is stream state: a thread publishes a delta on entering and leaving, and a consumer carries the running value forward.
`recording::from_trace(id)` does that carrying; `recording::trace_relations()` hands back the edges.

#### A trace is an async scope

A trace wants to be **infectious**: a request or a job spans threads, and every piece of work spawned under it belongs to it wherever it ends up running.
That is exactly what `cc::async`'s ambient chain does, so a trace scope IS an ambient scope carrying an id, and propagation, naming and the state deltas are one mechanism rather than two.

So there is no separate trace scope: `CC_RECORD_ASYNC_SCOPE` opens one, minting the id, and `CC_RECORD_ASYNC_SCOPE_WITH_ID` takes one from elsewhere.
`cc::rec::current_trace_id()` reads the chain, so it is correct on whichever worker resumed the work.

What stays here is minting, the relation vocabulary and recording an edge.
A relation gates on `category::tracing`; the scope itself is an async scope and gates on `category::profiling`.

### The console

`cc::rec::console_listener` turns log events back into lines, and is **deliberately a little behind in exchange for a total order**.
Blocks arrive from different threads in no particular order, so printing them as they land would interleave a multi-threaded run into nonsense.
One drain's worth is buffered, sorted by timestamp, and printed at the end of the batch.

Only `event_kind::log` reaches the terminal.
A console that also printed every scope and stat would be unreadable, and those have listeners of their own.

It is not installed for you — see the note at the top of this document.

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

## The chunk preamble

The current trace id is stream **state**, not a per-event field.
A thread emits a delta only when it changes, at the four sites in `cc::async` that install or adopt an ambient context, and a reader carries the running value forward.

Each chunk still has to be independently decodable, or ring capture and crash dumps could not start reading in the middle.
So **every chunk opens with an `event_kind::stream_state` event**, written by the producer at rotation.

It is the producer's rather than the consumer's because **an open scope cannot be derived**.
A trace could be — a consumer reading a thread in order has seen every delta — but a long-lived scope opens *once*.
Imagine one or two frame or worker scopes per thread, opened at startup and never re-opened.
A window that outlived their `scope_begin` — a ring buffer, a decimated capture, the tail of a crash dump — has nothing left to learn from, and would render everything inside them at the wrong depth.

The preamble is a fixed forty bytes, which is what keeps a rotation's cost independent of how deep the thread happens to be:

| field | meaning |
|---|---|
| `trace` | the trace id in effect |
| `scope_depth` | how many scopes are open, in full |
| `named_scopes` | how many of the three slots below are filled |
| `scope0`–`scope2` | the **outermost** open scopes, as `type_code::desc_ref` |

The two counts are deliberately separate.
A preamble reporting depth 7 with 2 names says exactly what it does and does not know, so a reader nests correctly and renders the rest unnamed rather than guessing.
Three slots covers the long-lived frames a bounded capture actually loses; anything deeper is short-lived enough that its own `scope_begin` is almost certainly still inside the window.

Outermost rather than innermost for the same reason: the outer frames are the ones that opened long ago and were evicted.

**`desc_ref` is the one payload field that is a pointer into this binary.**
Serializing rewrites it into a descriptor-table index and loading rewrites it back, exactly as an event header's own descriptor is.
So a preamble read out of a `.ccrec` names the scope against that file's own table.
Read one with `event_view::field_as_desc`, never by reinterpreting the bytes.

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

## The algebra, and asking questions of a recording

A recording is a value, so it composes.

```cpp
auto const mine   = captured.from_thread(cc::current_thread_id());
auto const errors = captured.filtered([](auto const&, auto const& e) { return e.level() >= cc::rec::level::error; });
auto const window = captured.in_cycle_range(begin, end);
auto const thin   = captured.decimated({.keep_from_cycles = cutoff});
```

Filtering **synthesizes** blocks rather than borrowing.
The events it keeps are no longer contiguous, so they are copied into a buffer the result owns.
Which also means a filtered recording stops pinning the chunks it came from, and stays readable long after the pool has recycled them.
Capturing still copies nothing; only narrowing does.

`decimated` exists to make a bounded-memory ring capture honest.
It drops what finished before the cutoff, and keeps scopes still open across it — otherwise a thinned trace loses the frame you were sitting inside.
What went is replaced by one `dropped_span` event per block, saying how many events it covered and over what span.
So after decimating, a reader can still tell "nothing happened" from "we stopped looking".

The queries are what turn a recording into a test assertion:

```cpp
r.count("cache-miss");                             // how many
r.contains("cache-miss");                          // any at all
r.first_value("vertex_count");                     // an optional<f64> — absent is not zero
r.last_value("queue_depth");  r.values("attempts");
r.first_text("path");                              // an optional<cc::string>
r.contains_in_order({"open", "write", "close"});   // anything allowed between, wrong order is false
r.messages();                                      // every log line, as it would print
r.scopes();  r.scopes("upload-pass");              // matched begin/end pairs, with durations
```

Everything ordered is ordered by **timestamp**, not by block, so a query means the same thing across threads as within one.

`scopes()` matches pairs per thread by name and depth, so two scopes sharing a name still nest correctly.
A scope whose close is missing comes back with `is_open` set rather than being dropped — a truncated stream is the normal case here, not an error.

Queries are linear scans.
An index would be a hash from descriptor to offsets built on first use; nothing has needed one yet, and it is recorded in [TODO](../TODO.md) rather than built.

### The assertion pattern

This is the shape that replaces a debug getter: the code under test records what it did, and the test asks.

```cpp
cc::rec::recording_listener rl;
{
    auto const h = cc::rec::register_listener(rl);
    load_mesh(cold_cache);
    cc::rec::flush_blocking();
    cc::rec::unregister_listener(h);
}
auto const r = rl.take().from_thread(cc::current_thread_id());

CHECK(r.contains("cache-miss"));
CHECK(r.first_value("vertex_count").value() == 1024);
CHECK(r.contains_in_order({"vertex_count", "cache-miss", "bytes_read"}));
```

Narrowing by thread is what makes this reliable **synchronously**.
Work that runs asynchronously needs the ambient context instead, since a logical task runs on whichever workers pick it up and several are in flight at once.

That is what nexus does: every `nx::run` stands a recorder up, mints a trace per test, and buckets each test's events by it, so a test reads its own back with `nx::test_recording()`.
[nexus/recording](../../../nexus/docs/recording.md) is that half, including what it costs and how to turn it off.

---

## What the recorder costs

The whole design bets that annotation which is cheap is annotation that stays in the code.
A bet like that is worth nothing without a number, and a number from someone else's machine is worth less than nothing.

```cpp
auto const model = cc::rec::measure_overhead();    // a few ms; records into the live system, in the cc.record domain
model.fixed_cycles;      // a zero-payload event
model.cycles_per_byte;   // the copy
model.disabled_cycles;   // one load through the domain and a test — what "leave it in forever" rests on

captured.estimated_overhead_cycles();
captured.estimated_overhead_ratio();               // over wall time summed across threads
```

The model is a straight line, `fixed + per_byte * payload`.
That is not a claim that a site is linear; it is a claim that the two things it does are the two terms worth fitting.
Anything else is either rare enough to measure itself, or already reported as a cold-path event.
A stacktrace capture carries its own end timestamp, and the estimate uses that in preference to the model.

`measure_overhead` reports the **minimum** per-event cost across several rounds rather than the mean.
A scheduling hiccup can only make a sample look slower, so the minimum estimates the cost and the mean estimates the cost plus the machine's mood.

Backpressure needs no separate accounting: a stall happens inside the cold path, and `record.chunk_acquired` already carries how long that took.

---

## Lifecycle constraints

`initialize()` must be called once; a second call asserts rather than reconfiguring.

`shutdown()` requires that **no other thread is recording**, and that every listener is already unregistered.
It invalidates every registered thread's write cursor before releasing the pool.
A cursor with room left never reaches the cold path, so no amount of checking there could stop a thread from writing into freed memory.
That is also why `thread_state` holds a pointer back into its owner's thread-local cursor.
It is the one thing in the system that reaches across threads that way, and it exists for exactly this.

That first requirement became much easier to violate once async scopes landed.
A thread publishes an ambient delta wherever `cc::async` restores a context.
So **any thread driving async work is a recording thread**, whether or not the code on it mentions `cc::rec` at all.
Tearing the recorder down while a thread pool still runs is the shape to watch for, and it is why the `cc::rec` tests run alone rather than under a shared exclusion tag.

A generation counter backs the same invariant on the cold path.
Both `initialize()` and `shutdown()` bump it, and a thread whose local copy is stale forgets everything it remembered about the previous incarnation.

Without threads (`SC_THREADS=OFF`, or `config::threaded = false`) no API changes: draining happens on whichever thread flushes, and latency is worse.

---

## What is here today

`cc::rec` carries the stream and the vocabularies over it: logging, profiling scopes, values, markers and stats, plus the console listener and the record algebra.

To LOOK at a recording rather than assert on one, `babel::chrome_trace` writes it as Chrome Trace Event JSON for `chrome://tracing` and `ui.perfetto.dev`.
`uv run dev.py example babel-serializer/chrome-trace` records a synthetic workload and writes one.

A recording also serializes, and a crash dump writes the same format without allocating — [systems/recording-formats](recording-formats.md) is that half.

Async scopes and their ambient deltas are here, so attribution follows work across a `co_await` — and tracing rides them, so a trace does too.

nexus turns that into test assertions: `nx::test_recording()` hands a test what it recorded, and a failing test's recording is written out.
[nexus/recording](../../../nexus/docs/recording.md) is that half.

`cc::capture_stack` ([stack_capture.hh](../../src/clean-core/platform/stack_capture.hh)) captures return addresses, and never symbolizes.
That is analysis, and it costs orders of magnitude more than the event it would hang off.

**What it costs differs by an order of magnitude across platforms**, and stacktrace policy should follow that.
Where a frame pointer is kept (`SC_FRAME_POINTERS`, so clang and GCC) the walk chases the chain at a few nanoseconds a frame.
Windows has no frame-pointer ABI on x64 and MSVC cannot maintain one, so it unwinds from tables instead: correct, and roughly a microsecond for a deep stack.
A stack on every error is affordable; a stack on every warning is not.

A capture can also stop early, at `cc::rec::current_scope_frame()`.
Everything below the frame that opened the innermost profiling scope is what the scope stack already names, so walking it again spends time and payload on a fact the stream carries.
That matters most for sampling, where it shortens the stacks rather than just the walk.
The scope guard stores that frame next to the depth it already touches, so the early-out costs one store per scope.

Emscripten has no walkable native stack and returns an empty capture; `cc::stack_capture_available()` says which you are in.

**Names come later, from `cc::symbolizer`** ([symbolize.hh](../../src/clean-core/platform/symbolize.hh)).
Capture writes addresses and never asks who they belong to, because asking costs orders of magnitude more than the event it would hang off.
So the asking happens at analysis time, and the result is cached — a sampled profile is thousands of hits on a handful of addresses.

**Which modules it resolves against is the choice.**
By default it is this process's own, which is right for a program exporting its own trace.
Given a recorded module table it resolves against THAT instead, in a debug-info session of its own — see [recording-formats](recording-formats.md#the-module-table).
That is what makes a recording from another run, or from a process that has since died, readable at all.

A foreign table is worth having even when the binaries are not.
Failing to load one costs the function and the line and still leaves the module and the offset, so a frame degrades to `app.exe+0x1234`.
An unresolved frame keeps its address rather than acquiring a confident wrong name.

### Changing what the sampler does, while it does it

The whole `sampling_config` is re-read once per tick, so `cc::rec::reconfigure_sampling` takes effect within one interval and nothing needs the sampler stopped.
That matters beyond convenience: a stop-and-restart loses every sample in the gap, which is exactly the stretch somebody turning a knob is looking at.

```cpp
cc::rec::reconfigure_sampling({.rate_hz = 2000.0});                  // a slider in a profiler window
cc::rec::sampling_override const all({.include_unknown_threads = true}); // ...or just around what you are debugging
```

**`include_unknown_threads` is off by default, because it costs the profile and not only the CPU.**
A tick walks every OS thread instead of the ones the recorder knows, and measured over half a second at 1 kHz:

| unknown threads | ticks | samples | mean tick | sampler load |
|---|---|---|---|---|
| off | 374 | 373 | 18 µs | 1.4% |
| on | 220 | 3989 | 167 µs | 7.4% |

The tick count is the number that matters.
Turning it on samples the threads you actually asked about **40% less often**, so it is a mode for hunting a thread that records nothing — which is exactly what it is for — rather than a default.

### Placing samples while the program runs

A sample is written to the sampler's stream carrying an anchor into the sampled thread's, and [`recording::spliced_samples`](../../src/clean-core/record/recording.hh) moves it home.
That works on a recording that is already complete.

A live consumer has no such luxury: it sees blocks as they are drained, and a sample's target may not have arrived yet.
[`cc::rec::splicing_listener`](../../src/clean-core/record/splicing_listener.hh) sits in front of one and trades a bounded delay for placement.

```cpp
cc::rec::splicing_listener splicer(my_listener);   // register THIS, not my_listener
```

Everything is forwarded at the end of the batch it arrived in; a sample whose target is missing is carried for `max_hold_batches` and then forwarded where it was recorded.
**Giving up is not losing** — that is exactly what the offline splice does with a sample it cannot place, and it is why splicing twice is a no-op.

Measured at 1 kHz over a burn loop, the live splicer places **every anchored sample**, matching the offline splice exactly at every hold setting:

| hold batches | samples | anchored | placed live | placed offline |
|---|---|---|---|---|
| 1 | 1598 | 128 | 128 | 128 |
| 4 | 1734 | 132 | 132 | 132 |
| 8 | 1721 | 135 | 135 | 135 |

The gap between `samples` and `anchored` is not a splicing failure.
A sample of a thread the recorder never heard of has no stream to be anchored into, and with `include_unknown_threads` on most samples are those — see [sampling](#sampling).

### Keeping a bounded window

A capture that runs for hours has to throw something away, and **which** thing depends entirely on what the capture is for.
A crash ring wants the recent past whatever it costs; a background trace wants a memory ceiling it never breaches.

Those are not one policy with different numbers, so [`cc::rec::retention_policy`](../../src/clean-core/record/recording.hh) expresses both:

```cpp
cc::rec::recording_listener bounded({.max_secs = 30, .max_bytes = 128 << 20});
cc::rec::recording_listener forensic({.guaranteed_secs = 20, .max_bytes = 64 << 20});
```

The first is two caps, and whichever binds first wins.
The second promises the last twenty seconds whatever they cost, and the byte cap is what gives way.

**`guaranteed_secs` is what makes a byte cap safe for forensics.**
Without it a burst of logging evicts the seconds before a crash, which is the only part anybody wanted.
`max_secs` outranks it where the two disagree, since a hard age limit is the stricter promise — otherwise a guarantee would silently disable a cap set right next to it.

Every limit is off at zero, so a default policy keeps everything and an ordinary scoped capture pays nothing.

**Retention is block-granular**, so a bound is approximate by up to one chunk.
A block is the unit a chunk reference keeps alive, and evicting anything smaller would free no memory at all.

**Every stack is written out in full, and a sample is self-contained.**
Interning them — writing each distinct stack once and having later samples carry an id — was tried and removed, because it does not compose with retention.
A definition is written once at the front of a capture, and a bounded ring evicts oldest-first, so a forensic ring would keep samples whose stacks it had already thrown away.
The size it saved does not pay for that.
`stop_at_scope` leaves an instrumented stack at one or two frames, where an id is no smaller than the frames it replaces, so only a deep uninstrumented stack ever benefited.

If sampled bytes do become the constraint, the shape to reach for is a stream-level dictionary that retention understands — not a sampler-local table.
That is the open TODO entry; there is no reader-side indirection to keep working around until then.

**`cc::rec::hot_functions`** ([hot_functions.hh](../../src/clean-core/record/hot_functions.hh)) is the reduction that makes all of this usable without a viewer.
It symbolizes every captured stack and folds them by function, into self time and inclusive time.

Self time — the innermost captured frame — is what a profile is for, and the split is what the report is worth:
a function high in inclusive time and absent from self time is a caller, and moving it would change nothing.

**Inclusive counts only cover what was captured.**
A sample stops at the innermost open profiling scope, so a stack under one is deliberately short and the scope spans carry the rest.
Reading the two together is the intent, and the exporter nests them for exactly that reason.

Being a value rather than a rendering is what makes it assertable: `self_ratio_of("decode")` in a test is a performance regression caught in CI rather than one noticed in a viewer.

**No binary format here is stable**, and none will be for a good while.
Durability comes from an exporter, not from the raw bytes.
