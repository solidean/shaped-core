# byte hash benchmark (xxHash)

Throughput of the raw xxHash entry points behind clean-core's hashing — `XXH3_64bits_withSeed` /
`XXH3_128bits_withSeed` — and the thin wrappers over them (`cc::make_hash_of_bytes` /
[`cc::hash128::create`](../../src/clean-core/common/hash128.hh)), over a key-length sweep. Unlike the
[string-hash benchmark](string-hash-benchmark.md), this hashes raw byte ranges, with no string/SSO layer.

Source: [tests/benchmarks/hash-benchmark.cc](../../tests/benchmarks/hash-benchmark.cc).

## Two findings came out of this

### 1. The RelWithDebInfo `/Ob1` trap (fixed project-wide)

The string-hash benchmark showed XXH3 running *catastrophically* slowly for short/mid keys in the **default
RelWithDebInfo** dev build — under ~1 GB/s up to ~216 bytes — while Release was fine. `CC_ASSERT` is ours, not
xxHash's, so that pointed at the build, not the library.

Cause: the compiler is `clang-cl`, and the *only* flag difference between the presets was the inlining level —
RelWithDebInfo defaulted to `/Ob1` (inline only functions explicitly marked `inline`), Release to `/Ob2`.
xxHash's short/mid-key path (keys ≤ `XXH3_MIDSIZE_MAX` = 240 bytes) is a chain of plain `static` helpers that
only collapse into the caller under `/Ob2`; under `/Ob1` each stays a real call.

Fix: the root [CMakeLists.txt](../../../../../CMakeLists.txt) now promotes RelWithDebInfo to `/Ob2`
project-wide (`add_compile_options($<$<CONFIG:RelWithDebInfo>:/Ob2>)`) — the mental model is *RelWithDebInfo =
Release codegen + assertions + debug info*, and `/Ob1` was a misleading default. Impact on raw `xxh64`:

| length | before /Ob1 | after /Ob2 | speedup× |
|------:|------:|------:|------:|
| 1 | 0.12 | 0.40 | 3.33 |
| 8 | 0.50 | 4.33 | 8.66 |
| 16 | 0.73 | 9.12 | 12.49 |
| 32 | 1.02 | 13.08 | 12.82 |
| 64 | 1.15 | 18.17 | 15.80 |
| 96 | 1.15 | 20.05 | 17.43 |
| 216 | 1.08 | 13.85 | 12.82 |
| 324 | 7.00 | 18.07 | 2.58 |

(The 240→324 jump in the *before* column is XXH3's documented switch from the scalar short/mid path to its
vectorized long-hash accumulator — `XXH3_MIDSIZE_MAX` in `xxhash.h`. That cliff is by design; `/Ob1` just made
everything below it pathologically slow.)

### 2. The wrappers needed a purity attribute

Even after the inlining fix, the wrapper columns (`hob64`/`hash128`) trailed the raw columns by more than a
single extra call should cost. The reason: the raw xxHash entry points are tagged `XXH_PUREF`
(`__attribute__((pure))`), which lets the compiler pipeline calls across the loop; the wrappers carried no
such promise, so the compiler treated every wrapper call as an opaque memory barrier and serialized the loop.

`cc::make_hash_of_bytes` and `cc::hash128::create` are now tagged `CC_PURE` (a new portable macro —
`[[gnu::pure]]` on GCC/Clang/clang-cl, empty on MSVC). For the 64-bit wrapper this recovered most of the gap
(RelWithDebInfo, 16-byte keys: `hob64` 6.9 → 7.8 GB/s, vs raw 8.7). A residual ~10% remains for tiny keys —
the genuinely irreducible cost of one non-inlined call (`make_hash_of_bytes` lives in its own TU and, without
LTO, cannot be inlined) — and it vanishes past a few dozen bytes.

The **128-bit** wrapper does *not* close up: it returns a 16-byte struct via a hidden pointer (sret), an extra
memory round-trip the `pure` tag cannot remove, so `hash128` stays meaningfully below raw `xxh128` for short
keys. Prefer the 64-bit hash for hash-table keys unless you actually need 128 bits.

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

This full table is the manual `bench-hash (… full sweep)` benchmark. A lean `GUIDE_BENCHMARK` of the same
base name records just the representative points (≈8 B and ≈64 KiB) via `nx::guide` for `dev.py pgo`. Both are
excluded from normal sweeps; the `"bench-hash"` filter matches both — name them explicitly:

```bash
uv run dev.py test "bench-hash" --target clean-core-test --preset release-clang --timeout 0
uv run dev.py test "bench-hash" --target clean-core-test --preset relwithdebinfo-clang --timeout 0
```

See [docs/guides/perf-results.md](../../../../../docs/guides/perf-results.md).

## Results

GB/s; higher is better. `hob64` = `cc::make_hash_of_bytes` (XXH3-64 wrapper), `hash128` = `cc::hash128::create`
(XXH3-128 wrapper), `xxh64`/`xxh128` = the raw xxHash calls.

### RelWithDebInfo (after global `/Ob2` + `CC_PURE`)

| length | hob64 | hash128 | xxh64 | xxh128 |
|------:|------:|------:|------:|------:|
| 1 | 0.38 | 0.12 | 0.40 | 0.23 |
| 2 | 0.74 | 0.23 | 0.81 | 0.47 |
| 3 | 1.11 | 0.34 | 1.21 | 0.70 |
| 4 | 1.93 | 0.48 | 2.13 | 1.19 |
| 5 | 2.45 | 0.60 | 2.69 | 1.48 |
| 6 | 2.94 | 0.72 | 3.31 | 1.82 |
| 7 | 3.51 | 0.85 | 3.82 | 2.13 |
| 8 | 4.01 | 0.98 | 4.33 | 2.42 |
| 9 | 4.55 | 1.05 | 5.07 | 2.49 |
| 10 | 5.08 | 1.19 | 5.71 | 2.76 |
| 11 | 5.56 | 1.31 | 6.28 | 3.05 |
| 12 | 6.09 | 1.43 | 6.80 | 3.32 |
| 13 | 6.58 | 1.55 | 7.41 | 3.58 |
| 14 | 7.11 | 1.67 | 7.97 | 3.88 |
| 15 | 7.59 | 1.79 | 8.55 | 4.16 |
| 16 | 8.07 | 1.86 | 9.12 | 4.45 |
| 17 | 6.47 | 1.88 | 7.07 | 3.77 |
| 18 | 6.81 | 2.00 | 7.47 | 3.99 |
| 19 | 7.22 | 2.10 | 7.90 | 4.17 |
| 20 | 7.45 | 2.19 | 8.20 | 4.40 |
| 21 | 7.93 | 2.31 | 8.58 | 4.65 |
| 22 | 8.36 | 2.44 | 9.02 | 4.85 |
| 23 | 8.77 | 2.52 | 9.37 | 5.08 |
| 24 | 8.99 | 2.64 | 9.85 | 5.36 |
| 25 | 9.49 | 2.72 | 10.30 | 5.54 |
| 26 | 9.75 | 2.83 | 10.61 | 5.75 |
| 27 | 10.12 | 2.97 | 10.61 | 5.66 |
| 28 | 9.90 | 2.89 | 11.24 | 5.99 |
| 29 | 10.58 | 3.07 | 11.98 | 6.26 |
| 30 | 10.98 | 3.25 | 11.89 | 6.46 |
| 31 | 11.39 | 3.32 | 12.45 | 6.58 |
| 32 | 12.03 | 3.53 | 13.08 | 6.90 |
| 40 | 11.32 | 4.02 | 11.49 | 6.88 |
| 48 | 13.12 | 4.60 | 13.40 | 7.72 |
| 56 | 14.93 | 5.56 | 16.21 | 9.58 |
| 64 | 18.06 | 6.34 | 18.17 | 10.55 |
| 96 | 19.99 | 8.89 | 20.05 | 12.51 |
| 144 | 10.71 | 9.39 | 11.01 | 11.90 |
| 216 | 13.48 | 11.42 | 13.85 | 14.55 |
| 324 | 17.54 | 12.12 | 18.07 | 15.23 |
| 486 | 22.40 | 16.98 | 22.65 | 19.26 |
| 729 | 25.49 | 20.70 | 25.74 | 23.15 |
| 1093 | 26.84 | 23.03 | 26.98 | 25.46 |
| 1639 | 30.10 | 27.07 | 30.23 | 28.94 |
| 2458 | 31.51 | 29.52 | 31.58 | 30.81 |
| 3687 | 32.97 | 31.66 | 33.13 | 32.19 |
| 5530 | 33.90 | 33.03 | 33.91 | 33.74 |
| 8295 | 34.52 | 33.91 | 33.88 | 34.39 |
| 12442 | 34.57 | 34.43 | 34.55 | 34.89 |
| 18663 | 35.09 | 35.10 | 33.94 | 35.21 |
| 27994 | 34.67 | 35.51 | 34.77 | 35.14 |
| 41991 | 35.00 | 35.58 | 34.56 | 35.36 |
| 62986 | 35.20 | 35.96 | 35.62 | 36.03 |
| 94479 | 35.67 | 34.91 | 35.60 | 36.20 |

### Release

| length | hob64 | hash128 | xxh64 | xxh128 |
|------:|------:|------:|------:|------:|
| 1 | 0.42 | 0.12 | 0.46 | 0.26 |
| 2 | 0.83 | 0.24 | 0.91 | 0.51 |
| 3 | 1.25 | 0.35 | 1.36 | 0.75 |
| 4 | 1.74 | 0.50 | 2.04 | 1.25 |
| 5 | 2.24 | 0.63 | 2.45 | 1.56 |
| 6 | 2.76 | 0.75 | 3.06 | 1.90 |
| 7 | 3.22 | 0.88 | 3.56 | 2.20 |
| 8 | 3.56 | 1.01 | 4.10 | 2.55 |
| 9 | 4.52 | 1.06 | 5.04 | 2.43 |
| 10 | 4.97 | 1.17 | 5.61 | 2.70 |
| 11 | 5.61 | 1.31 | 6.29 | 2.92 |
| 12 | 6.09 | 1.43 | 6.82 | 3.24 |
| 13 | 6.61 | 1.55 | 7.42 | 3.45 |
| 14 | 7.12 | 1.66 | 8.00 | 3.77 |
| 15 | 7.64 | 1.78 | 8.54 | 4.00 |
| 16 | 8.12 | 1.90 | 9.15 | 4.32 |
| 17 | 7.06 | 1.89 | 7.72 | 3.81 |
| 18 | 7.46 | 2.00 | 8.22 | 4.03 |
| 19 | 7.87 | 2.10 | 8.26 | 4.07 |
| 20 | 7.97 | 2.19 | 8.77 | 4.31 |
| 21 | 8.45 | 2.32 | 9.29 | 4.53 |
| 22 | 8.96 | 2.41 | 9.81 | 4.58 |
| 23 | 8.93 | 2.40 | 9.86 | 4.76 |
| 24 | 9.31 | 2.51 | 10.33 | 4.99 |
| 25 | 9.24 | 2.54 | 10.18 | 5.55 |
| 26 | 10.45 | 2.89 | 11.84 | 5.81 |
| 27 | 11.15 | 2.96 | 11.87 | 5.82 |
| 28 | 11.63 | 3.11 | 12.72 | 6.18 |
| 29 | 11.28 | 2.99 | 12.95 | 6.46 |
| 30 | 12.43 | 3.31 | 13.65 | 6.69 |
| 31 | 12.83 | 3.43 | 14.08 | 6.91 |
| 32 | 13.23 | 3.54 | 14.58 | 7.07 |
| 40 | 11.40 | 4.10 | 11.71 | 6.45 |
| 48 | 13.65 | 4.90 | 14.07 | 7.80 |
| 56 | 15.90 | 5.72 | 16.32 | 9.11 |
| 64 | 18.16 | 6.56 | 18.74 | 10.39 |
| 96 | 19.87 | 8.84 | 19.98 | 12.73 |
| 144 | 11.12 | 9.57 | 11.34 | 12.05 |
| 216 | 13.70 | 11.56 | 13.83 | 14.79 |
| 324 | 17.99 | 12.81 | 17.94 | 15.18 |
| 486 | 22.47 | 16.92 | 22.67 | 19.69 |
| 729 | 25.52 | 20.62 | 25.64 | 23.32 |
| 1093 | 27.08 | 23.07 | 27.03 | 25.22 |
| 1639 | 30.25 | 26.73 | 30.26 | 28.66 |
| 2458 | 31.89 | 28.82 | 31.80 | 30.33 |
| 3687 | 31.48 | 29.13 | 31.23 | 30.16 |
| 5530 | 31.38 | 30.25 | 32.17 | 30.53 |
| 8295 | 35.08 | 33.58 | 34.27 | 33.83 |
| 12442 | 35.00 | 33.86 | 35.11 | 34.25 |
| 18663 | 35.54 | 34.56 | 35.44 | 34.43 |
| 27994 | 35.79 | 34.99 | 35.96 | 34.91 |
| 41991 | 35.98 | 35.20 | 36.05 | 35.22 |
| 62986 | 36.26 | 35.47 | 36.14 | 35.40 |
| 94479 | 36.43 | 35.40 | 36.29 | 35.48 |

## Takeaways

- **Promoting RelWithDebInfo to `/Ob2` was the big fix** — short-key xxHash went from ~0.7 to ~8 GB/s at
  16 bytes (~11×) and now matches Release.
- **`CC_PURE` on the wrappers** recovers most of the remaining wrapper overhead by letting the compiler
  pipeline wrapper calls; a fixed ~10% per-call cost survives for tiny keys (no LTO across the clean-core TU
  boundary) and amortizes away past a few dozen bytes.
- **64-bit beats 128-bit for short keys** (roughly 2× the GB/s up to ~32 bytes); the 128-bit wrapper also
  pays an sret struct-return cost the purity tag can't remove. Use the 64-bit hash for hash-table keys.
- **The 240-byte cliff is xxHash by design** (`XXH3_MIDSIZE_MAX`): scalar mixing below, the SIMD long-hash
  path above, plateauing ~30–36 GB/s.
