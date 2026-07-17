# File stream benchmark: `cc` adapters vs `std::fstream`

Throughput of the clean-core file stream adapters
([file_stream.hh](../../src/clean-core/streams/file_stream.hh)) against
`std::ofstream` / `std::ifstream`, across I/O **granularity**. Source:
[tests/benchmarks/file-stream-benchmark.cc](../../tests/benchmarks/file-stream-benchmark.cc).

**Headline:** driving single bytes through the buffer window, the cc streams are
**~11× faster to write and ~16× faster to read** than `std`'s `put`/`get`; the lead
then narrows smoothly as the record grows (write parity around 64 B, reads
cc-favoured throughout) and reaches parity at bulk. The win is the exposed buffer
window — small reads/writes inline to a pointer bump + `memcpy` with no per-operation
call, where `std`'s `put()`/`get()` pay a streambuf sentry + virtual dispatch on
every byte.

## Method

Each timed pass is end-to-end — **open → transfer 4 MiB → close** — repeated by the
adaptive timer ([bench_util.hh](../../tests/benchmarks/bench_util.hh)) for ~50 ms.
Granularities per direction: 1 byte via `put`/`get`, then 2 / 4 / 8 / 16 / 64 / 256 B
and 64 KiB via `write`/`read`. The cc 1-byte path uses the window directly
(`writable_bytes()`/`produce()`, `ready_bytes()`/`consume()`) and `std` uses
`ofstream::put` / `ifstream::get`; every larger size uses `write`/`read` on both.
Files are opened binary.

`bench::measure_units_per_sec` **discards one warmup pass per measurement** before it
times, so each metric faults its own code path and warms its file before timing;
repeated passes then stay in the page / write-back cache. So the number measures the
**stream layer's CPU cost, not the disk**. (nexus itself runs the benchmark body once
— no framework-level repeat or warmup; the per-metric warmup is the bench helper's.)
At 4 MiB per pass the open/close is well under a percent of the time.

## Results

Windows, clang-cl (Clang 21), `release` preset. MB/s (decimal), warm cache; median
of several runs, which agree to ~±15%.

1 B is `put`/`get`; every larger size is `write`/`read` (of an N-byte span).

| granularity      | cc write | std write | cc read | std read | cc/std write | cc/std read |
|------------------|---------:|----------:|--------:|---------:|-------------:|------------:|
| 1 B (`put`/`get`) |    ~500 |      ~46 |  ~1090 |     ~68 |      **~11×** |    **~16×** |
| 2 B              |     ~345 |      ~72 |   ~470 |    ~113 |         ~4.8× |       ~4.2× |
| 4 B              |     ~485 |     ~137 |   ~755 |    ~213 |         ~3.5× |       ~3.5× |
| 8 B              |     ~575 |     ~235 |  ~1160 |    ~375 |         ~2.4× |       ~3.1× |
| 16 B             |     ~570 |     ~384 |  ~1520 |    ~615 |         ~1.5× |       ~2.5× |
| 64 B             |     ~570 |     ~586 |  ~2040 |   ~1260 |         ~1.0× |       ~1.6× |
| 256 B            |     ~590 |     ~570 |  ~2220 |   ~1900 |        ~1.03× |       ~1.2× |
| 64 KiB           |     ~545 |     ~577 |  ~2280 |   ~2145 |        ~0.95× |      ~1.05× |

(The first run right after a rebuild reads 2–4× low across the *whole* table — the
machine is still busy from the parallel build and not at turbo. That is machine
state, not per-metric cache: the bench helper's per-measurement warmup already covers
the cache, and no in-benchmark warmup fixes the machine being busy. Run on an idle
machine and discard the first post-build run.)

## Analysis

- **1 B via the window is the win (~11× / ~16×).** `cc` exposes `[curr, end)`
  directly, so a single-byte write is `writable_bytes()` (two loads), a store, and
  `produce(1)` (a pointer bump) — all inlined; the type-erased flush pointer is touched
  only when the 4 KiB buffer fills, i.e. once per 4096 bytes. `std::ofstream::put`
  constructs an `ostream::sentry` and dispatches through the virtual `streambuf` **on
  every byte**; `ifstream::get` likewise. That per-operation tax is the gap.

- **The 2 B dip is the API boundary, and it's instructive.** At 2 B the cc path
  switches from the hand-rolled window loop (1 B) to the general `write()`/`read()`,
  which carries a fixed per-call cost (validity assert, loop setup, `min`, the
  `first_write` check, span/`result` plumbing). Over ~2M calls that drops cc's 2 B
  write *below* its own 1 B window path (~345 vs ~500) — while still ~4–5× over `std`.
  **Takeaway:** for the hottest byte-oriented loops, drive the window directly
  (`writable_bytes()`/`produce()`, `ready_bytes()`/`consume()`); `write()`/`read()` is for
  records, not single bytes.

- **The gap closes smoothly as the record grows.** From 2 → 64 B the per-call cost
  amortizes: cc write climbs to ~570 and `std` catches up, reaching **write parity
  around 64 B**; past that `std` is a hair ahead on write (its bulk path is slightly
  leaner). Reads stay `cc`-favoured across the whole range (~2.5× at 16 B, ~1.2× at
  256 B), converging to parity by 64 KiB.

- **Bulk (64 KiB): parity.** Both are a `memcpy` into the buffer plus the syscalls to
  drain/fill it; for cached I/O the copy + write-back dominates. `cc`'s smaller (4 KiB)
  buffer issues more, smaller syscalls, but that is not the bottleneck here, so the two
  land within noise (`std` a hair ahead on write, `cc` on read).

### A note on very large, *uncached* transfers

For transfers far larger than the buffer that miss the cache (so the syscall count
actually bites), a **bulk bypass** — when a single `read`/`write` request exceeds the
buffer capacity, drain the buffer once and then transfer the caller's span straight
to/from the OS in one call instead of in 4 KiB buffer-sized pieces — would cut `cc`'s
syscall count to match `std` (which already bypasses its buffer for large requests).
That optimization is **not implemented today**; the parity above is measured on
cached I/O where it would not have shown up. Worth adding if bulk file copies become a
hot path.

## Running it

```bash
# guide benchmark: prints the table, records the 1 B points into the .perf.json sidecar
uv run dev.py --mirror-output test "bench-file-stream (cc vs std)" --preset release-clang
```

Use a `release` preset (asserts off, optimized) and an otherwise-idle machine;
discard the first post-build run.
