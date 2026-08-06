# clean-core docs

Documentation hub for clean-core.
For the library overview, public types, and how to include headers, start at the [readme](../readme.md).
For repo-wide docs see [docs/_index.md](../../../../docs/_index.md).

## Source organization

clean-core's headers live in `src/clean-core/`, grouped by topic:

```text
clean-core/
  fwd.hh        # forward declarations of the public types
  common/       # macros, utility/meta, flags, hash, assertions
  platform/     # console (color), native (demangling), source_location, stacktrace, win32_sanitized
  math/         # bit utilities, the random PRNG, wide_arith's 128-bit primitives
  memory/       # allocation, node_allocation, shared_ptr (+ impl/)
  container/    # array/vector families, map, set, span, strided_span, … (+ impl/)
  sequence/     # the lazy ranges API (early prototype)
  string/       # string, string_view, char_predicates, format, formatter, print, to_string, to_debug_string, conversion
  function/     # function_ref, unique_function
  error/        # optional, result, crash_handler
  thread/       # async + its work-stealing pool, threaded_actor, atomic, mutex, spin, thread
```

`impl/` subfolders are private implementation details.
The [readme](../readme.md#file-organization) has the full per-folder table.

## Topics

- [containers](containers.md) — the contracts every container type shares: how to choose one, what `T` must be,
  what indexing checks, and when references and iterators die.
- [strings](strings.md) — `cc::string` and `cc::string_view`: null-termination and C interop, when a pointer dies,
  how storage moves between inline and heap, and what hashes equal to what.
- [formatting](formatting.md) — `cc::format` and the `to_string` / `to_debug_string` alternatives.
  The placeholder grammar, the `cc::custom::formatter<T>` protocol, and what the compile-time check does and does not cover.
- [sequence](sequence.md) — `cc::sequence`, the lazy forward cursor over a range.
  An early prototype: the doc separates the reductions that work today from the design the rest of the API is intended to follow.
- [blessed-stdlib-headers](blessed-stdlib-headers.md) — the standard headers clean-core is allowed to depend on directly, and why.
- [customization-points](customization-points.md) — the `cc::custom::` trait + hidden-friend protocol that operations like hashing use to let types opt in.
- [writing-a-stream](writing-a-stream.md) — how to add your own byte-stream adapter.
  The `cc::seek_dir` / flush contract, a minimal worked example, and the buffered / write / read_write cases.
- [benchmarks/string-hash-benchmark](benchmarks/string-hash-benchmark.md) — XXH3 vs hand-rolled short-string hashers across a length sweep,
  which is the small-key cost in hash maps.
- [benchmarks/hash-benchmark](benchmarks/hash-benchmark.md) — raw xxHash 64/128 vs the wrappers.
  The `clang-cl /Ob1` inlining trap that crippled short-key hashing in dev builds, and the `CC_PURE` attribute that frees the wrapper.
- [benchmarks/allocation-benchmark](benchmarks/allocation-benchmark.md) — mimalloc vs the system allocator across sizes.
  mimalloc leads at every size and is only mildly `/Ob1`-sensitive.
- [benchmarks/file-stream-benchmark](benchmarks/file-stream-benchmark.md) — the file stream adapters vs `std::fstream` across a granularity sweep.
  ~11×/16× faster single-byte via the buffer window, narrowing to parity as records grow.
- [benchmarks/async-benchmark](benchmarks/async-benchmark.md) — the four `cc::async` benchmarks: the per-node tax on one thread, and where a born-ready node's ~40 ns goes.
  Plus pool scaling across five fork-join shapes, and the leaf size at which fork-join overhead stops dominating.

Add further deep-dive docs here as kebab-case `.md` files and link them from this list.

## Systems

Deep dives on the internal machinery, including holes and gotchas not obvious from the headers:

- [systems/allocation](systems/allocation.md) — `cc::allocation<T>`, the owning storage handle under `array` and `vector`, and the `memory_resource` interface.
  The extract/adopt escape hatch across container types works today; `retype` and the ergonomic API around it do not yet.
- [systems/node-allocation](systems/node-allocation.md) — the slab allocator for small nodes: size classes, wait-free cross-thread free, and the slab lifecycle across thread exit and adoption.
- [systems/shared-ptr](systems/shared-ptr.md) — `cc::shared_ptr` / `cc::weak_ptr`, the 8 B intrusive-refcount handle pair over one slab node.
  The Traits protocol is provisional and shaped by async's needs; the lifetime and release/adopt contracts are not.
- [systems/async](systems/async.md) — `cc::async<T, E>`, the value/dataflow async.
  The frame model, the never-blocking poll loop, the 64 B node layout, the work-stealing pool, and what a node costs.

## Conventions

- Namespace `cc`; **no dependencies** (bottom of the library stack).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md);
  `.clang-format` is authoritative for formatting.
