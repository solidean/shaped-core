# clean-core TODO

Running list of known follow-ups.
Add entries as we discover them, and remove them as they land.

## common

- **Enum value names, through `cc::custom::enum_traits`.**
  A first version prints `enum_name(integer_value)`, and `cc::flags::to_string` then prints a comma-separated list of them.
  That needs a variadic enumerator list in the macro plus a name table on the traits, next to `is_flag_enum` and `flag_storage_type` — which is the shape the traits already have.
  The catch is that only ONE specialization of `cc::custom::enum_traits<E>` may exist, so a names-only macro cannot sit alongside a `CC_FLAG_ENUM_*` one for the same enum.
  The two have to converge into one macro that declares everything, with `is_flag_enum` telling a nameable enum from a flag enum.
- **Migrate the remaining hand-rolled flag enums to `cc::flags`.**
  `sr::key_modifiers` is the last one still carrying its own operators plus a free `has_all`.
  All of sg's have migrated — `buffer_usages`, `texture_usages`, `access_flags`, `pipeline_stage_flags`, `color_write_mask`, `accel_build_flags`.

- **Revisit the regular-void stance across the vocabulary types.**
  Today `void` is rejected and you spell `cc::unit` by hand: `result<void, E>` is a static_assert pointing at it, `optional<void>` has the same shape, and `async<void>` was never wired up at all.
  Generic code that forms `xyz<T>` where `T` can reasonably be `void` hits this everywhere, so a per-type alias fixes one corner and leaves the rest.
  The direction is to support `xyz<void>` as an affordance meaning `xyz<unit>` — same type, no conversions — decided once for `result`, `optional` and `async` together rather than type by type.
  Two consequences the async side already ran into, and that the decision has to answer.
  A `shared_async<void>` dependency should contribute an ordering edge and NO argument to the `make_async_*` sugar.
  And it becomes indistinguishable from one deliberately carrying a `unit`.

- **`cc::is_finite` (and friends) for floats.**
  There is no float classification in cc at all, so a caller that must not emit a NaN hand-rolls one.
  `babel::json`'s writer is the first, with an exponent-bit check in json_writer.cc — exact, and needing no `<cmath>`, which is not blessed there.
  A `cc::is_finite` / `is_nan` / `is_inf` trio next to the other scalar utilities would take all three call sites this will grow.

## platform

- **`cc::symbolizer` is not thread-safe, and the symbolize tests flake because of it.**
  `platform/symbolize.cc` calls `SymInitialize`, `SymFromAddr` and `SymGetLineFromAddr64` with no lock of any kind.
  Microsoft documents every DbgHelp entry point as single-threaded — concurrent calls give "unexpected behavior or memory corruption".
  `platform/native.cc` right beside it already takes a process-wide mutex around the demanglers for exactly that reason, so the pattern is established and simply was not applied here.
  Two symbolizers on two threads therefore race.
  The symptom is a resolve that returns the WRONG function rather than an error, which is the failure mode hardest to recognize as a race.

  Seen on 2026-08-26, on Windows x64 `relwithdebinfo`, during a full `dev.py check`.
  That runs the four presets' test binaries in parallel, and nexus runs tests on a pool on top of that.
  It failed once, passed on an immediate re-run, and passes on `--repeat 10` in isolation on the same tree.
  The comment in `tests/platform/symbolize-test.cc` records an earlier failure on arm64 CI, so this is that one again rather than a new thing.
  What is new is that it reproduces on x64 and has an explanation.

  The `nx::config::recorded` sidecar is what settled it, and it is worth reading before touching this.
  The failing run's `.ccrec` shows all 13 frames walked and resolved, and every one correct except frame 0.
  That frame must be inside `capture_here_for_symbolize_test`, and it resolved instead to `cc::impl::unique_function_invoke<lambda ...>` — a function from an entirely different translation unit.
  The failure message also names the two sibling symbolize tests running concurrently, which is the contention.

  The fix is the one `native.cc` already uses: a process-wide mutex around the DbgHelp calls.
  Held for the whole `resolve` rather than per call, since `SymFromAddr` and `SymGetLineFromAddr64` are two reads of one session's state.
  It costs nothing anybody would notice, because symbolization is already the slow path.
  It is also what makes a crash dump written from several threads trustworthy, which matters more than the test does.

- **There is no way to read a `.ccrec` from the command line.**
  Diagnosing the entry above meant scraping printable runs out of the file with a throwaway script.
  `cc::rec::load_recording` is the whole reader and nothing exposes it.
  A `dev.py` subcommand that dumps a recording's events is what would make the sidecar pay off the way `nx::config::recorded` intends.

## container

- **`bitset` printing and allocation interop.**
  Neither bit set has a `to_string`: it would drag `cc::string` into a container header, the way `variant`'s missing one does.
  The bit order is already decided — **index 0 leftmost**, since a bit set is an indexed bit array rather than a number.
  `cc::bitset` also cannot yet adopt or extract its `cc::allocation<u64>`.
- **`ringbuffer` adoption and extraction.**
  It holds its `cc::allocation<T>` as a raw byte handle, so neither `create_from_allocation` nor `extract_allocation` exists yet.
  Adoption can take the largest power of two at or below the incoming capacity; extraction needs the content linearized first, or a check that it already is.
- **Grow `tuple` and `variant`.**
  The first version deliberately left out converting construction from another `tuple<Us...>`, `tuple_cat`, `variant`'s `operator<=>` and multi-variant visitation.
  A `to_string` hidden friend for `variant` is missing too — it would drag `to_debug_string.hh` into a container header, so a `variant` currently debug-prints as a raw byte dump.

## memory

- **A build mode where every node is a real allocation, so a leaked node is visible to a leak checker.**
  A small-class node comes out of a 64-slot slab, so a leaked one is unused bytes inside a slab that stays reachable — LeakSanitizer has nothing to report, in every configuration.
  `SC_MIMALLOC=OFF` made the *slabs* visible; nodes sit one level below where it reaches.
  It is not free either: `node_trim_ring` reclaims only a *fully* free slab, so one leaked 32-byte node pins its whole 2 KB slab for the life of the process.
  The mechanism already exists as the large-node path.
  `node_allocator::allocate_node_bytes` branches at `idx > small_max` to `allocate_node_bytes_large`, one `cc::default_memory_resource` allocation per node.
  `node_allocation_free_large` mirrors it on the way out.
  So the mode is that branch always taken, under a `CC_NODE_ALLOC_DIRECT` derived from a CMake input the way `CC_HAS_MIMALLOC` is.
  Compile-time rather than a runtime flag, because the branch sits in a `CC_FORCE_INLINE` function on the hottest path clean-core has.
  Set it in the `sanitize-*` presets, beside `SC_MIMALLOC=OFF`.
  Verify first that nothing outside the slab paths recovers a slab base from a node pointer.
  `ptr & ~node_slab_mask_for_class(idx)` holds for a slot and not for a directly-allocated node.
  The cost to state when it lands: `align_up(node_large_header_size, alignment)` turns a 1-byte class-0 node into a 24-byte allocation.
  So the mode is a memory multiplier as well as a speed one.

## bytes

- **base64.**
  `babel::base64` exists and is text-oriented: it takes a `cc::string_view`, tolerates whitespace and both alphabets, and returns a `cc::string`.
  A byte-level codec belongs here instead, and moving babel's would drag its tolerant-input contract down with it — the two want different answers on malformed input.
  Blocked on somebody below babel needing it; nobody does yet.
- **CRC32 and Adler32.**
  Wanted by every container format that stores a per-member checksum — zip, gzip, PNG — so this lands with the first of those rather than before it.
  Neither is a hash in the `common/hash.hh` sense and neither is cryptographic; they belong next to the codecs, not next to `blake3`.
  Both now exist in the tree, vendored with zlib and reached only by `deflate_backend.cc`; exposing them is a matter of deciding the API, not of finding an implementation.
- **Further compression algorithms.**
  Deflate landed over vendored zlib — `compression_algorithm::deflate`, with `frame` meaning gzip and a third `compression_framing::zlib` for the RFC 1950 wrapper.
  Nothing else is queued; brotli would be the next interop codec if some format demanded it, and would slot in the same way.
- **Async compression.**
  `compress_async`, mirroring `algorithm/sort_async.hh`: explicit extra API over the synchronous core, never a hidden worker pool.
  zstd's own multithreading is deliberately not enabled, so this would chunk over `cc::async` instead and keep the memory cost where the caller can see it.
- **Error correction.**
  Reed-Solomon or similar, for bytes that have to survive a corrupted medium rather than merely get smaller.
  Speculative — nothing here needs it yet, and it is listed so the folder's shape is on record rather than because it is planned.

## streams

- **Revisit what happens to a buffered write stream's tail.**
  Nothing auto-flushes today, so bytes still in the window when the adapter dies are simply lost, and the file ends on a `k_buffer_size` boundary.
  Every whole-file caller pays a `flush()` for it, and `nexus::run` even carries a comment saying why.
  `cc::rec::save_serialized_recording` sidesteps the adapter entirely, writing through `native_file` in a hand-rolled loop.
  The first caller to forget it lost the tail of a JPEG, which still decoded: a truncated scan is flat-filled from the last value read, so it read as a bug in whatever produced the pixels.
  **A silent failure whose symptom points somewhere else is the thing to fix**, and the fix is not obvious.
  A destructor cannot do it: the write position lives in the stream, and the adapter holds only the buffer, so it has nothing to flush with.
  Moving it into the stream's destructor puts a non-trivial destructor on a type whose whole point is that every operation inlines.
  It also swallows the one error — a full disk — that must not be swallowed.
  A whole-file helper was considered and rejected: too easy to get wrong, and it does not fit the filesystem abstraction this is heading toward.
  So this is recorded as an open question rather than a plan.

## cc::rec

- **A query index over a recording.**
  The queries are linear scans today, which is fine to somewhere around a million events and no test comes close.
  An index would be a hash from descriptor to event offsets plus a sorted timestamp array, built on first query and cached on the recording.
- **A richer value codec.**
  `CC_RECORD` takes scalars, enums, pointers and text; `desc::fields` already describes multi-field payloads, and nothing has needed one yet.
- **An interned tier for recorded text.**
  The static tier landed: a literal VALUE is stored as its address (`type_code::cstring`), and only a runtime string is copied.
  What is left is `cc::interned_string`, whose 8-byte handle would give a runtime string the same cost as a literal with no lifetime rule attached.
  It is also what would let `CC_RECORD_NAMED` take a runtime string value, since the handle is fixed-size.
- **Debounced and rate-limited logging.**
  There is no `CC_LOG_*_ONCE` and no `log_once` helper, so every "warn once" site hand-rolls its own flag — sg alone has several, and the transfer work is about to add two more.
  The shapes wanted are once-per-call-site and at-most-once-per-interval, both keyed on the site rather than on a caller-supplied token, since a per-resource key is the caller's business.
  Until it exists, a caller carries its own flag beside the state the warning is about.
- **Sampling above the OS timer's ceiling.**
  A high-resolution waitable timer floors near half a millisecond on Windows — about 1.9 kHz — and going faster means sleeping short and spinning the remainder, which burns a core for the privilege.
  Worth it only for a short, deliberate capture, so it wants to be an explicit mode rather than a rate that quietly starts spinning.
- **A stack dictionary that retention understands.**
  Samples write their frames out in full, so a kilohertz across many uninstrumented threads is megabytes a second of repeated return addresses.
  A sampler-local intern table was tried and removed: definitions are written once at the front of a capture, and a bounded ring evicts oldest-first, so the ring outlives the stacks it refers to.
  Whatever replaces it has to be a property of the stream rather than of the sampler — a dictionary retention keeps, or re-emits, so that any surviving window is self-contained.
- **A POSIX sampler.**
  Windows suspends a thread and walks it from outside; POSIX has no equivalent.
  There it wants `SIGPROF` via `timer_create`, plus a handler that walks its own stack and hands the frames and the anchor to the sampler thread.
  The event layout and the splice are already shared, so only the capture half differs.

## async

- **The async-vs-direct tax in `tests/benchmarks/async/async-benchmark.cc` is not believable.**
  A 512-node chain reports 423x over the direct analog, off a direct baseline of 0.36 ns per step — under one cycle for a call plus an add.
  The direct side is almost certainly folding despite the XOR-into-sink, so the column is measuring the optimizer rather than the machinery, and every tax in that file is suspect by the same argument.
  The `noinline` probes exist for the disassembly, not for the timed loop, which is the gap.
  The scalar `nx::bench::sink` is worth suspecting first: it claims one value is observed and nothing else, so a direct
  side that accumulates into something the compiler can carry in a register across the whole loop still folds, with the
  guard doing exactly what it documents.
  That would make the fix a `void(isize)` body laundering the accumulator per iteration rather than anything about
  `noinline`.
  Revisit once the benchmark architecture has settled rather than patching the numbers now.
