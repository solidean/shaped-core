# Compression

Two algorithms behind one API, and the choice between them is not the interesting decision.
The ones that actually change what you get are the level, whether the blob is small enough to need a dictionary, and whether the framing is worth its bytes.
All three go the opposite way from the usual advice once the payload is a few hundred bytes rather than a few megabytes.

Every number here comes from [tests/benchmarks/compression-benchmark.cc](../../tests/benchmarks/compression-benchmark.cc), run on payloads shaped-core actually stores.
That matters: upstream publishes its numbers on Silesia, a corpus of tarred novels and executables, and nothing here looks like that.

Run it yourself with `uv run dev.py test "bench-compress" --preset release-clang --timeout 0 --manual`.

## A small blob does not compress at all, dictionary or nothing

This is the single most useful thing on this page.

A ~72-byte record — one vdoc property assignment, one small JSON object — comes out **larger** than it went in:

| config | packed | ratio |
|---|---|---|
| zstd 3, no dictionary | 81 B | 0.89x |
| lz4 0, no dictionary | 99 B | 0.73x |
| **zstd 3, trained dictionary** | **23 B** | **3.13x** |

There is no level that fixes this, because the problem is not effort.
A codec compresses by referring backwards, and 72 bytes end before there is anything to refer back to.
A dictionary is that backward reference, trained once over a corpus and handed to every call — see [compression_dictionary.hh](../../src/clean-core/bytes/compression_dictionary.hh).

Note that zstd 19 with the dictionary produced exactly the same 23 bytes as zstd 3 with it, at a third of the speed.
Once the dictionary supplies the matches, the level has nothing left to find.

**So: a format storing many small blobs should train a dictionary before it considers anything else on this page.**

## zstd 3 is the default and usually the answer

On ~256 kB of JSON-ish records:

| level | ratio | compress | decompress |
|---|---|---|---|
| zstd -5 | 4.18x | 2276 MB/s | 5330 MB/s |
| zstd -1 | 4.67x | 1844 MB/s | 5008 MB/s |
| zstd 3 | 6.96x | 1087 MB/s | 3323 MB/s |
| zstd 9 | 7.22x | 175 MB/s | 3474 MB/s |
| zstd 19 | 8.62x | **6.1 MB/s** | 3871 MB/s |

Level 19 costs **180x the compression time of level 3 for 24% better ratio**.
That is worth it for something written once and read forever — a shipped asset, a release artifact — and worth it almost nowhere else.

Level 9 is the shape of the curve's knee: +4% ratio for 6x the time.
Above it the returns are small and the cost is not.

`level = 0` and `level = 3` produce byte-identical output, because 0 means "the algorithm's default" and zstd's default is 3.

## lz4 buys decompression speed, and it is not free

lz4 never won on ratio at comparable speed in any measurement here.
zstd -1 beat lz4 0 on both axes at once (4.67x at 1844 MB/s against 3.94x at 2345 MB/s is close enough that the ratio decides it).

What lz4 does buy is the decompression side, and on small blobs the margin is large:

| config | ratio | decompress |
|---|---|---|
| zstd raw, 16 records (1147 B) | 4.28x | 985 MB/s |
| lz4 raw, 16 records (1147 B) | 2.85x | **24 979 MB/s** |

That is 25x the decompression throughput for a third less ratio.
**Reach for lz4 when a blob is decompressed far more often than it is written and the bytes are cheap** — a runtime cache, a per-frame resource.
Reach for zstd otherwise.

lz4's high-compression levels are the one configuration to avoid outright: level 12 compressed at 21.6 MB/s for 4.85x, where zstd 3 got 6.96x at 1087 MB/s.
If lz4's ratio is not good enough, the answer is zstd, not lz4 HC.

## Raw framing is worth more for lz4 than for zstd

`raw` strips the self-describing header — see [compression.hh](../../src/clean-core/bytes/compression.hh) for what the two backends each leave behind.

On a single 72-byte record it saves 4 bytes under zstd (81 → 77) and **25 bytes under lz4** (99 → 74), because lz4's frame header is much the heavier of the two.

But the size is not the main reason to reach for it.
Raw lz4 decompressed a 1147-byte blob at 25 GB/s against the frame path's 2.6 GB/s, because the block API skips the per-call frame context and the checksum entirely.
**On small blobs the framing choice moves decompression throughput by an order of magnitude, and the byte count is the smaller half of the argument.**

The price is that a raw blob describes nothing about itself.
The format has to record the algorithm, and for lz4 the uncompressed size as well.

## Spending level on incompressible data is pure waste

The bytecode-like payload — structured binary, not text — compressed to 1.34x at zstd 3 and 1.36x at zstd 19.
The whole level range bought 2%.

zstd's negative levels did not compress it at all (1.00x) but ran at 10 GB/s, which is the fast modes correctly deciding there is nothing here and falling through to storing it.
lz4 did the same at every level.

**Measure before paying for a level.** A payload with little redundancy gives the same answer at every setting, and the only thing the level changes is how long you wait for it.

## What this is not

**It is not a benchmark suite.**
The numbers above are a handful of points on one machine, taken to answer specific questions.
They are not tracked over time, and a change that moves them is not automatically a regression — [perf-results](../../../../../docs/guides/perf-results.md) is what tracked metrics look like.

**It is not a guide to the API.**
[compression.hh](../../src/clean-core/bytes/compression.hh) owns the contracts, and the [cheat sheet](../../cheat-sheet.md) is the fast recall.

**It is not about throughput at scale.**
Everything here is synchronous and single-threaded on purpose.
A caller wanting more than one core chunks the work over `cc::async` itself, which keeps that decision — and its memory cost — where the caller can see it.
