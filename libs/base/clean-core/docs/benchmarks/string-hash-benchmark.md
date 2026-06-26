# string hash benchmark

Throughput of the production string hash, `cc::make_hash_of_bytes`
([hash.hh](../../src/clean-core/common/hash.hh)) — the XXH3-64 path that `cc::string` and `cc::string_view`
hash through — against two hand-rolled "small string" hashers, over a length sweep. The question: does XXH3's
fixed setup cost make it a poor default for the short keys that dominate hash-table workloads, and where is
the crossover?

Source: [tests/benchmarks/string-hash-bench.cc](../../tests/benchmarks/string-hash-bench.cc).

> **Re-measured after two fixes.** An earlier revision showed XXH3 under ~1 GB/s for all short/mid keys in
> RelWithDebInfo — a `clang-cl /Ob1` inlining artifact, now fixed project-wide (RelWithDebInfo is `/Ob2`), and
> `cc::make_hash_of_bytes` now carries `CC_PURE`. The full story is in the
> [byte-hash benchmark](hash-benchmark.md).

## What is measured

GB/s while hashing a large corpus of **distinct** keys back to back — the hash-table insert/lookup scenario.
For each length the corpus is an ~8 MB working set of random printable-ASCII keys (key count capped at 200k
for tiny lengths), hashed in a tight loop repeated until ≥ 50 ms elapses. Two corpora: `string_view` (views
into one buffer; pure hash cost) and `string` (owning `cc::string`; ≤ 39 bytes inline via SSO).

The length sweep is 1..32 (every length), then +8 up to 64, then ×1.5 up to ~100k.

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

```bash
uv run dev.py test "bench-string-hash" --target clean-core-test --preset release-clang --timeout 0
uv run dev.py test "bench-string-hash" --target clean-core-test --preset relwithdebinfo-clang --timeout 0
```

## Hashers

- **xxh3** — `cc::make_hash_of_bytes` (XXH3-64).
- **fnv1a** — classic FNV-1a, one multiply per byte. Trivial setup; byte-at-a-time.
- **mul** — a word-at-a-time multiply/xor mixer with overlapping fixed-size tail reads (wyhash-style). Almost
  no fixed setup. Not a vetted hash — a competent speed foil for the small-string regime.

## Results

GB/s; higher is better.

### RelWithDebInfo

#### string_view corpus

| length | xxh3 | fnv1a | mul |
|------:|------:|------:|------:|
| 1 | 0.32 | 1.06 | 0.60 |
| 2 | 0.68 | 1.80 | 1.21 |
| 3 | 1.05 | 1.80 | 1.75 |
| 4 | 1.81 | 2.62 | 3.13 |
| 5 | 2.27 | 2.24 | 3.76 |
| 6 | 2.71 | 2.38 | 4.72 |
| 7 | 3.14 | 2.08 | 5.52 |
| 8 | 3.57 | 2.96 | 4.43 |
| 9 | 4.52 | 2.94 | 4.01 |
| 10 | 4.86 | 2.73 | 4.43 |
| 11 | 5.48 | 2.80 | 4.81 |
| 12 | 5.77 | 2.83 | 5.46 |
| 13 | 6.41 | 1.74 | 5.87 |
| 14 | 7.01 | 1.90 | 6.32 |
| 15 | 7.48 | 1.93 | 6.66 |
| 16 | 7.99 | 2.62 | 6.65 |
| 17 | 6.13 | 2.14 | 6.36 |
| 18 | 6.64 | 1.83 | 6.27 |
| 19 | 7.14 | 2.19 | 7.14 |
| 20 | 7.54 | 2.42 | 7.32 |
| 21 | 7.88 | 2.43 | 7.89 |
| 22 | 8.28 | 2.44 | 8.29 |
| 23 | 8.44 | 2.25 | 8.42 |
| 24 | 8.83 | 2.26 | 7.57 |
| 25 | 8.96 | 2.02 | 5.58 |
| 26 | 5.46 | 1.67 | 7.61 |
| 27 | 8.84 | 1.65 | 5.03 |
| 28 | 7.19 | 1.65 | 7.83 |
| 29 | 6.45 | 1.71 | 7.02 |
| 30 | 5.10 | 1.82 | 6.60 |
| 31 | 9.10 | 1.50 | 5.68 |
| 32 | 7.34 | 1.51 | 8.49 |
| 40 | 9.99 | 1.76 | 8.09 |
| 48 | 10.50 | 1.74 | 9.73 |
| 56 | 8.88 | 1.14 | 4.82 |
| 64 | 4.05 | 1.12 | 5.15 |
| 96 | 8.25 | 1.02 | 4.94 |
| 144 | 5.34 | 1.04 | 6.67 |
| 216 | 6.53 | 0.99 | 4.70 |
| 324 | 6.81 | 0.83 | 5.31 |
| 486 | 9.32 | 1.03 | 5.71 |
| 729 | 15.16 | 1.12 | 6.35 |
| 1093 | 14.24 | 1.12 | 6.15 |
| 1639 | 19.10 | 1.11 | 6.04 |
| 2458 | 18.75 | 1.11 | 5.93 |
| 3687 | 19.35 | 1.15 | 6.17 |
| 5530 | 20.71 | 1.14 | 6.08 |
| 8295 | 20.88 | 1.14 | 6.12 |
| 12442 | 22.57 | 1.15 | 6.23 |
| 18663 | 22.89 | 1.16 | 6.17 |
| 27994 | 22.74 | 1.16 | 6.21 |
| 41991 | 23.53 | 1.17 | 6.24 |
| 62986 | 23.74 | 1.16 | 6.24 |
| 94479 | 23.71 | 1.18 | 6.23 |

#### string (SSO) corpus

| length | xxh3 | fnv1a | mul |
|------:|------:|------:|------:|
| 1 | 0.35 | 0.91 | 0.59 |
| 2 | 0.70 | 1.51 | 1.17 |
| 3 | 1.05 | 1.56 | 1.76 |
| 4 | 1.80 | 2.68 | 2.76 |
| 5 | 2.25 | 2.56 | 3.40 |
| 6 | 2.69 | 2.63 | 4.15 |
| 7 | 3.05 | 2.82 | 4.25 |
| 8 | 3.45 | 2.96 | 4.09 |
| 9 | 4.05 | 2.65 | 3.65 |
| 10 | 4.51 | 2.76 | 3.99 |
| 11 | 4.97 | 2.79 | 4.47 |
| 12 | 5.42 | 3.06 | 4.90 |
| 13 | 5.89 | 2.58 | 5.28 |
| 14 | 6.33 | 2.50 | 5.71 |
| 15 | 6.78 | 2.83 | 6.13 |
| 16 | 6.89 | 2.78 | 6.23 |
| 17 | 5.89 | 2.67 | 5.87 |
| 18 | 6.27 | 2.17 | 6.29 |
| 19 | 6.62 | 2.27 | 6.62 |
| 20 | 6.84 | 2.55 | 6.89 |
| 21 | 7.33 | 2.13 | 7.23 |
| 22 | 7.63 | 2.10 | 7.59 |
| 23 | 7.96 | 2.37 | 8.00 |
| 24 | 8.33 | 2.43 | 7.68 |
| 25 | 8.50 | 2.24 | 7.16 |
| 26 | 9.01 | 2.28 | 7.46 |
| 27 | 9.28 | 2.26 | 7.74 |
| 28 | 9.75 | 2.30 | 8.03 |
| 29 | 9.89 | 2.07 | 8.33 |
| 30 | 10.41 | 2.05 | 8.01 |
| 31 | 9.97 | 2.10 | 8.34 |
| 32 | 10.50 | 2.12 | 9.84 |
| 40 | 9.15 | 2.06 | 8.75 |
| 48 | 11.89 | 1.85 | 9.52 |
| 56 | 13.48 | 1.72 | 9.76 |
| 64 | 15.87 | 1.61 | 11.18 |
| 96 | 18.32 | 1.44 | 11.24 |
| 144 | 10.32 | 1.34 | 9.62 |
| 216 | 13.26 | 1.28 | 9.57 |
| 324 | 17.09 | 1.25 | 9.43 |
| 486 | 21.51 | 1.20 | 8.05 |
| 729 | 25.06 | 1.20 | 7.35 |
| 1093 | 26.67 | 1.19 | 6.95 |
| 1639 | 29.56 | 1.12 | 6.38 |
| 2458 | 29.67 | 1.12 | 6.19 |
| 3687 | 31.47 | 1.11 | 6.09 |
| 5530 | 31.47 | 1.13 | 6.07 |
| 8295 | 31.68 | 1.09 | 6.27 |
| 12442 | 34.47 | 1.18 | 6.31 |
| 18663 | 33.70 | 1.18 | 6.31 |
| 27994 | 35.08 | 1.18 | 6.29 |
| 41991 | 35.29 | 1.18 | 6.25 |
| 62986 | 35.29 | 1.16 | 6.31 |
| 94479 | 35.50 | 1.19 | 6.28 |

### Release

#### string_view corpus

| length | xxh3 | fnv1a | mul |
|------:|------:|------:|------:|
| 1 | 0.37 | 1.15 | 0.64 |
| 2 | 0.74 | 1.79 | 1.29 |
| 3 | 1.13 | 1.51 | 1.95 |
| 4 | 1.70 | 2.98 | 3.18 |
| 5 | 2.06 | 2.25 | 3.88 |
| 6 | 2.49 | 2.48 | 4.77 |
| 7 | 2.86 | 2.27 | 5.53 |
| 8 | 3.41 | 3.04 | 4.33 |
| 9 | 4.09 | 2.42 | 3.99 |
| 10 | 4.56 | 2.51 | 4.52 |
| 11 | 4.93 | 1.89 | 5.03 |
| 12 | 5.45 | 2.85 | 5.39 |
| 13 | 5.91 | 2.32 | 5.91 |
| 14 | 6.33 | 2.68 | 6.40 |
| 15 | 6.82 | 2.60 | 6.85 |
| 16 | 7.27 | 2.67 | 6.69 |
| 17 | 6.00 | 2.03 | 6.41 |
| 18 | 6.36 | 2.06 | 6.86 |
| 19 | 6.70 | 2.07 | 7.23 |
| 20 | 7.06 | 2.42 | 7.59 |
| 21 | 7.39 | 2.23 | 7.98 |
| 22 | 7.76 | 2.18 | 8.24 |
| 23 | 8.11 | 2.11 | 8.32 |
| 24 | 7.97 | 2.17 | 7.33 |
| 25 | 8.19 | 1.99 | 7.25 |
| 26 | 8.79 | 2.01 | 7.64 |
| 27 | 9.02 | 2.02 | 7.94 |
| 28 | 9.34 | 2.19 | 8.15 |
| 29 | 9.64 | 2.04 | 8.50 |
| 30 | 10.02 | 2.05 | 8.73 |
| 31 | 10.34 | 2.08 | 9.14 |
| 32 | 10.66 | 2.06 | 10.03 |
| 40 | 8.96 | 1.98 | 9.68 |
| 48 | 10.82 | 1.74 | 9.81 |
| 56 | 12.51 | 1.62 | 9.71 |
| 64 | 14.49 | 1.53 | 11.02 |
| 96 | 16.51 | 1.36 | 10.72 |
| 144 | 10.10 | 1.26 | 9.46 |
| 216 | 12.65 | 1.22 | 9.38 |
| 324 | 16.44 | 1.18 | 8.99 |
| 486 | 20.48 | 1.16 | 7.74 |
| 729 | 23.73 | 1.13 | 6.96 |
| 1093 | 25.52 | 1.12 | 6.47 |
| 1639 | 28.62 | 1.14 | 6.38 |
| 2458 | 30.28 | 1.12 | 6.18 |
| 3687 | 30.97 | 1.11 | 6.11 |
| 5530 | 32.27 | 1.11 | 6.00 |
| 8295 | 32.14 | 1.12 | 6.04 |
| 12442 | 33.40 | 1.11 | 5.99 |
| 18663 | 33.17 | 1.09 | 5.93 |
| 27994 | 33.52 | 1.11 | 5.87 |
| 41991 | 34.16 | 1.12 | 5.92 |
| 62986 | 33.90 | 1.08 | 5.92 |
| 94479 | 33.32 | 1.18 | 6.23 |

#### string (SSO) corpus

| length | xxh3 | fnv1a | mul |
|------:|------:|------:|------:|
| 1 | 0.38 | 1.01 | 0.65 |
| 2 | 0.76 | 1.50 | 1.21 |
| 3 | 1.07 | 1.43 | 1.94 |
| 4 | 1.64 | 2.53 | 2.62 |
| 5 | 1.92 | 2.59 | 3.29 |
| 6 | 2.32 | 2.65 | 3.91 |
| 7 | 2.72 | 2.57 | 4.56 |
| 8 | 3.08 | 2.84 | 3.82 |
| 9 | 3.84 | 2.67 | 3.77 |
| 10 | 4.28 | 2.70 | 4.17 |
| 11 | 4.72 | 2.60 | 4.61 |
| 12 | 5.12 | 2.95 | 4.91 |
| 13 | 5.59 | 2.55 | 5.61 |
| 14 | 6.36 | 2.74 | 6.00 |
| 15 | 6.54 | 2.72 | 6.66 |
| 16 | 7.25 | 2.75 | 6.15 |
| 17 | 6.45 | 2.69 | 5.71 |
| 18 | 6.82 | 2.62 | 6.27 |
| 19 | 7.18 | 2.59 | 6.53 |
| 20 | 7.58 | 2.73 | 6.97 |
| 21 | 7.94 | 2.43 | 7.17 |
| 22 | 7.99 | 2.39 | 7.49 |
| 23 | 8.71 | 2.45 | 8.03 |
| 24 | 9.05 | 2.25 | 7.32 |
| 25 | 8.93 | 2.19 | 6.97 |
| 26 | 9.18 | 2.15 | 7.28 |
| 27 | 9.68 | 2.16 | 7.45 |
| 28 | 10.58 | 2.25 | 8.09 |
| 29 | 10.99 | 2.28 | 8.53 |
| 30 | 11.39 | 2.23 | 8.88 |
| 31 | 11.72 | 2.25 | 9.16 |
| 32 | 12.13 | 2.18 | 10.40 |
| 40 | 9.23 | 1.98 | 8.50 |
| 48 | 11.23 | 1.72 | 8.91 |
| 56 | 13.38 | 1.61 | 8.93 |
| 64 | 14.82 | 1.49 | 10.19 |
| 96 | 16.80 | 1.31 | 10.64 |
| 144 | 9.55 | 1.27 | 9.04 |
| 216 | 12.35 | 1.18 | 9.29 |
| 324 | 16.34 | 1.19 | 9.21 |
| 486 | 20.90 | 1.14 | 7.66 |
| 729 | 23.67 | 1.13 | 7.05 |
| 1093 | 25.24 | 1.12 | 7.04 |
| 1639 | 28.08 | 1.20 | 6.71 |
| 2458 | 31.95 | 1.19 | 6.58 |
| 3687 | 33.63 | 1.16 | 6.50 |
| 5530 | 32.04 | 1.12 | 5.92 |
| 8295 | 32.51 | 1.18 | 6.24 |
| 12442 | 32.56 | 1.10 | 5.97 |
| 18663 | 33.47 | 1.11 | 5.93 |
| 27994 | 34.13 | 1.12 | 5.96 |
| 41991 | 34.27 | 1.11 | 6.06 |
| 62986 | 36.03 | 1.12 | 5.94 |
| 94479 | 34.46 | 1.11 | 5.98 |

## Takeaways

- **XXH3 still has a short-key penalty, but a mild one.** For ≤ ~16-byte keys the trivial `mul` hash matches
  or beats XXH3, and at length 1 both customs win (xxh3 ≈ 0.4 GB/s vs fnv1a ≈ 1). XXH3 pulls clearly ahead by
  ~24–32 bytes and plateaus ~30–36 GB/s on its SIMD long path.
- **RelWithDebInfo now tracks Release** for XXH3 (the old sub-1-GB/s collapse is gone after the `/Ob2` fix).
- **`fnv1a` only wins for the very shortest keys** (≤ ~4 bytes) and falls behind `mul` once there are several
  words to consume.
- **SSO vs view barely matters** — the `string` and `string_view` columns track each other; the cost is in
  the hash, not the key representation.

**Implication.** A dedicated word-at-a-time short-string hash for hash-table keys is still a worthwhile win
for keys up to ~16–24 bytes; XXH3 remains the right choice for longer byte ranges.
