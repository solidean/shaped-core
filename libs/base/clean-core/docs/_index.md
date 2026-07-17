# clean-core docs

Documentation hub for clean-core. For the library overview, public types, and how
to include headers, start at the [readme](../readme.md). For repo-wide docs see
[docs/_index.md](../../../../docs/_index.md).

## Source organization

clean-core's headers live in `src/clean-core/`, grouped by topic:

```text
clean-core/
  fwd.hh        # forward declarations of the public types
  common/       # macros, utility/meta, flags, hash, assertions
  platform/     # native (demangling), source_location, stacktrace
  math/         # bit utilities
  memory/       # allocation, node_allocation (+ impl/)
  container/    # array/vector families, map, set, span, strided_span, … (+ impl/)
  sequence/     # the lazy ranges API
  string/       # string, string_view, char_predicates, to_string, to_debug_string
  function/     # function_ref, unique_function
  error/        # optional, result
  thread/       # mutex
```

`impl/` subfolders are private implementation details. The
[readme](../readme.md#file-organization) has the full per-folder table.

## Topics

- [blessed-stdlib-headers](blessed-stdlib-headers.md) — the standard headers
  clean-core is allowed to depend on directly, and why.
- [customization-points](customization-points.md) — the `cc::custom::` trait +
  hidden-friend protocol that operations like hashing use to let types opt in.
- [writing-a-stream](writing-a-stream.md) — how to add your own byte-stream adapter:
  the `cc::seek_dir` / flush contract, a minimal worked example, and the buffered /
  write / read_write cases.
- [benchmarks/string-hash-benchmark](benchmarks/string-hash-benchmark.md) — XXH3 vs
  hand-rolled short-string hashers across a length sweep (the small-key cost in hash maps).
- [benchmarks/hash-benchmark](benchmarks/hash-benchmark.md) — raw xxHash 64/128 vs the
  wrappers; the `clang-cl /Ob1` inlining trap that crippled short-key hashing in dev builds, and
  the `CC_PURE` attribute that frees the wrapper.
- [benchmarks/allocation-benchmark](benchmarks/allocation-benchmark.md) — mimalloc vs the
  system allocator across sizes; mimalloc leads at every size and is only mildly `/Ob1`-sensitive.
- [benchmarks/file-stream-benchmark](benchmarks/file-stream-benchmark.md) — the file stream
  adapters vs `std::fstream` across a granularity sweep: ~11×/16× faster single-byte via the
  buffer window, narrowing to parity as records grow.

Add further deep-dive docs here as kebab-case `.md` files and link them from this
list.

## Systems

Deep dives on the internal machinery, including holes and gotchas not obvious from the headers:

- [systems/allocation](systems/allocation.md) — `cc::allocation<T>`, the owning storage handle
  under `array`/`vector`/`devector`, and the `memory_resource` interface. The extract/adopt escape
  hatch across container types works today; `retype` and the ergonomic API around it do not yet.
- [systems/node-allocation](systems/node-allocation.md) — the slab allocator for small nodes:
  size classes, wait-free cross-thread free, and the by-design slab leak in the current refill path.
- [systems/async](systems/async.md) — `cc::async<T, E>`, the value/dataflow async: the frame model, the
  never-blocking poll loop, the 64 B node layout, the work-stealing pool, and the direct-path cost breakdown.

## Conventions

- Namespace `cc`; **no dependencies** (bottom of the library stack).
- Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md);
  `.clang-format` is authoritative for formatting.
