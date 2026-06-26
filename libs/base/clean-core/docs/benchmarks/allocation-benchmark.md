# allocation benchmark (mimalloc vs system)

Throughput of the mimalloc-backed default resource (`cc::default_memory_resource`) against the platform
malloc/free resource (`cc::system_memory_resource`,
[allocation.hh](../../src/clean-core/memory/allocation.hh)), across allocation sizes. The pattern is a churn:
64 concurrently-live blocks, each cycle freeing the oldest and allocating a fresh one (the common
short-lived-object workload), touching both ends of each block to fault the pages.

Source: [tests/benchmarks/allocation-benchmark.cc](../../tests/benchmarks/allocation-benchmark.cc).

This is the companion check to the [byte-hash benchmark](hash-benchmark.md): does the vendored mimalloc suffer
the same RelWithDebInfo `/Ob1` under-inlining penalty as xxHash? It does, but only mildly — its hot
`malloc`/`free` are force-inlined upstream, so `/Ob1` cannot fully de-optimize them (~1.6× isolated, vs
xxHash's ~11×). The project-wide `/Ob2` promotion (see the hash benchmark) covers it regardless.

## System under test

| | |
|---|---|
| CPU | AMD Ryzen 9 5900X @ 4.7 GHz |
| Memory | DDR4-2666 |
| OS | Windows 11 |
| Compiler | Clang/`clang-cl` (`relwithdebinfo-clang` / `release-clang` presets) |

Single-run numbers; short-key/small-block rows are sub-millisecond and noisy (~10%). Read trends, not third
decimals.

## Reproducing

This full table is the manual `bench-alloc (… full sweep)` benchmark. A lean `GUIDE_BENCHMARK` of the same
base name records just the representative points (mimalloc/system at 64 B and 4 KiB) via `nx::guide` for
`dev.py pgo`. Both are excluded from normal sweeps; the `"bench-alloc"` filter matches both — name them
explicitly:

```bash
uv run dev.py test "bench-alloc" --target clean-core-test --preset release-clang --timeout 0
uv run dev.py test "bench-alloc" --target clean-core-test --preset relwithdebinfo-clang --timeout 0
```

See [docs/guides/perf-results.md](../../../../../docs/guides/perf-results.md).

## Results

Millions of alloc+free cycles per second; higher is better.

### RelWithDebInfo

| size (bytes) | mimalloc | system |
|------:|------:|------:|
| 16 | 117.0 | 29.5 |
| 32 | 119.6 | 29.6 |
| 64 | 120.8 | 30.1 |
| 128 | 104.5 | 29.5 |
| 256 | 111.1 | 30.0 |
| 512 | 54.7 | 28.8 |
| 1024 | 52.5 | 27.8 |
| 4096 | 42.8 | 29.0 |
| 16384 | 44.6 | 12.2 |
| 65536 | 13.3 | 10.8 |

### Release

| size (bytes) | mimalloc | system |
|------:|------:|------:|
| 16 | 151.0 | 31.0 |
| 32 | 141.9 | 32.0 |
| 64 | 159.4 | 31.7 |
| 128 | 138.3 | 31.2 |
| 256 | 145.1 | 31.0 |
| 512 | 60.0 | 31.3 |
| 1024 | 61.4 | 30.9 |
| 4096 | 50.6 | 30.6 |
| 16384 | 49.7 | 13.8 |
| 65536 | 14.7 | 12.2 |

## Takeaways

- **mimalloc dominates the system allocator at every size measured** — ~4–5× for small/medium blocks
  (≈160 vs ≈31 Mops/s at 16–64 bytes in Release), confirming the default resource is the right default.
- **The margin narrows with size**: by 64 KiB the churn pushes mimalloc onto its large-object path and the
  lead shrinks to ~1.2×, but mimalloc still edges the system allocator there — there is no crossover.
- **mimalloc was only mildly `/Ob1`-sensitive** (~1.6× isolated, vs xxHash's ~11×); the project-wide `/Ob2`
  promotion lifts the RelWithDebInfo numbers toward Release with no allocator-specific change needed.
