# Compression

Three algorithms behind one API, and the choice between them is not the interesting decision.
Deflate is not really a choice at all — it is what you reach for when some other format has already made it for you.
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

## deflate is dominated on every axis, and that is the point

zstd 3 beats deflate 6 on all three measurements at once, on the same ~256 kB of JSON-ish records.
This table is from a later run than the ones above, so read its throughputs against each other and not against theirs — the ratios are deterministic and do agree across both.

| config | ratio | compress | decompress |
|---|---|---|---|
| deflate 1 | 5.74x | 269 MB/s | 734 MB/s |
| deflate 6 | 6.52x | 84 MB/s | 799 MB/s |
| deflate 9 | 6.66x | 25 MB/s | 729 MB/s |
| **zstd 3** | **6.96x** | **533 MB/s** | **2152 MB/s** |

Better ratio, 6.3x the compression speed and 2.7x the decompression speed.
There is no payload shape in these measurements where deflate is the right answer on the merits, and that is not a defect — a 1996 format losing to a 2015 one is the expected result.

**So the rule is simple: never pick deflate to store our own bytes.**
Pick it when a zip, a gzip, a PNG stream or an HTTP `Content-Encoding` has already decided, which is the entire reason it is here.

One number does cut the other way.
Deflate's decompression is the slowest of the three by a wide margin — 799 MB/s against lz4's 3.3 GB/s — so a format that reads far more often than it writes pays for the interoperability every time.

## Raw deflate is the only thing that shrinks a 72-byte record

The framings cost exactly what their wrappers weigh, and on a single ~72-byte record that is the whole file:

| config | packed | ratio |
|---|---|---|
| deflate gzip | 88 B | 0.82x |
| deflate zlib | 76 B | 0.95x |
| **deflate raw** | **70 B** | **1.03x** |
| lz4 raw | 74 B | 0.97x |
| zstd raw | 77 B | 0.94x |

gzip's 10-byte header plus its CRC-32 and length come to 18 bytes, the zlib wrapper's two bytes plus an Adler-32 come to 6, and raw carries nothing.
Raw deflate is the only configuration measured here that came out smaller than it went in without a dictionary.
That is a curiosity rather than advice: [the dictionary section](#a-small-blob-does-not-compress-at-all-dictionary-or-nothing) gets 3.13x on the same record.

At 16 records (1147 B) raw deflate also took the best ratio of any framing measured, 4.33x against raw zstd's 4.28x, at a fifth of zstd's decompression speed and one fiftieth of raw lz4's.

## Raw framing is worth more for lz4 than for zstd

`raw` strips the self-describing header — see [compression.hh](../../src/clean-core/bytes/compression.hh) for what each of the three backends leaves behind.

On a single 72-byte record it saves 4 bytes under zstd (81 → 77) and **25 bytes under lz4** (99 → 74), because lz4's frame header is much the heavier of the two.

But the size is not the main reason to reach for it.
Raw lz4 decompressed a 1147-byte blob at 25 GB/s against the frame path's 2.6 GB/s, because the block API skips the per-call frame context and the checksum entirely.
**On small blobs the framing choice moves decompression throughput by an order of magnitude, and the byte count is the smaller half of the argument.**

The price is that a raw blob describes nothing about itself.
The format has to record the algorithm, and for lz4 the uncompressed size as well.

Deflate is the one algorithm with three wrappers rather than two, so `framing` carries a third value for it.
`frame` is gzip, `zlib` is the RFC 1950 wrapper that PNG's IDAT stream and most `Content-Encoding: deflate` payloads actually are, and `raw` is a bare deflate stream.
Only gzip is sniffable, its `1f 8b` being a real magic; the zlib wrapper opens with a checksum constraint instead, so a format storing one has to record that it did.

A `.gz` is a sequence of members rather than one stream, which `cat a.gz b.gz`, pigz and bgzip all produce, and decoding one decodes them all.
The declared size then covers only the last member, which is the second reason gzip's is a hint rather than a size.
A decompressing stream holds one window rather than the trailer, so it reports no size hint for gzip at all.

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
