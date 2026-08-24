# Writing a stream

clean-core's byte streams ([streams/stream.hh](../src/clean-core/streams/stream.hh)) are an **extension point**.
The six stream types are non-owning views; the buffering and the actual I/O live in an *adapter* that you can write yourself.
This guide shows how.
Hub: [_index.md](_index.md).

## The two pieces

- **A stream** (`cc::read_stream`, `cc::write_stream`, `cc::read_write_stream`, and the `seekable_` variants) is a non-owning, move-only view.
  It holds only a window into a buffer plus one callback: `{ byte* curr; byte* end; flush_fn flush; void* context; }`.
  Write-capable streams also carry `first_write`, and read_write streams also carry `write_end`.
  All the hot-path methods — `ready_bytes()/consume()`, `read()`, `writable_bytes()/produce()`, `write()` — operate *directly* on `curr`/`end`, so they inline to plain pointer moves and `memcpy`.
  **Only `flush` is a real call.**
- **An adapter** is the owning type *you* write.
  It holds the buffer and the I/O state, supplies the one flush callback, and hands out a stream over its buffer.
  clean-core ships three: spans ([span_stream.hh](../src/clean-core/streams/span_stream.hh)), files ([file_stream.hh](../src/clean-core/streams/file_stream.hh)),
  and growing in-memory buffers ([growing_stream.hh](../src/clean-core/streams/growing_stream.hh)).
  That is how you would add one for a socket, a compressor, or a ring buffer.

The authoring contract — `cc::seek_dir` and the `cc::stream_flush_fn` signature — is public in [streams/stream_flush.hh](../src/clean-core/streams/stream_flush.hh).

## A minimal adapter (unbuffered, in-memory, read-only)

The whole span is available at once, so this adapter is seekable and never refills.
It is essentially `cc::span_read_stream_adapter` written out:

```cpp
#include <clean-core/streams/stream.hh>

class my_span_reader
{
public:
    explicit my_span_reader(cc::span<cc::byte const> data)
      : _base(const_cast<cc::byte*>(data.data())), _size(data.size()) // read path never writes through
    {
    }

    // Hand out a stream over our buffer. The whole span is valid up front, so curr=base, end=base+size.
    // `this` becomes the stream's context; it must outlive the stream.
    [[nodiscard]] cc::seekable_read_stream stream()
    {
        return cc::seekable_read_stream(_base, _base + _size, &flush, this);
    }

private:
    static cc::result<cc::i64> flush(cc::byte*& curr, cc::byte*& end, cc::byte*& /*write_end*/, void* ctx,
                                     cc::i64 offset, cc::seek_dir dir, cc::byte* /*first_write*/)
    {
        auto& self = *static_cast<my_span_reader*>(ctx);
        cc::byte* const base = self._base;
        cc::isize const size = self._size;
        cc::i64 const pos = cc::i64(curr - base); // current position

        auto go = [&](cc::i64 target) -> cc::result<cc::i64>
        {
            if (target < 0 || target > size)
                return cc::error("out of range");
            curr = base + target; // reposition within the fully-in-memory span...
            end = base + size;    // ...end is always the end of the data
            return target;
        };

        switch (dir)
        {
        case cc::seek_dir::relative:     return go(pos + offset); // (relative, 0) is the plain flush
        case cc::seek_dir::begin:        return go(offset);
        case cc::seek_dir::end:          return go(size + offset);
        case cc::seek_dir::dry_relative: return pos + offset;     // dry: report only, do not mutate
        case cc::seek_dir::dry_begin:    return offset;
        case cc::seek_dir::dry_end:      return size + offset;
        case cc::seek_dir::remaining_size_hint: return size - pos;  // a count, not a position
        }
        CC_UNREACHABLE("invalid seek_dir");
    }

    cc::byte* _base;
    cc::isize _size;
};
```

That is a complete, seekable read stream.
`my_span_reader(bytes).stream()` gives a `cc::seekable_read_stream` you can `read()`, `seek_to()`, `size()`, and so on.

## The flush contract

`flush` is the sole callback, and its signature is in [stream_flush.hh](../src/clean-core/streams/stream_flush.hh):

```cpp
cc::result<cc::i64> flush(cc::byte*& curr, cc::byte*& end, cc::byte*& write_end,
                          void* ctx, cc::i64 offset, cc::seek_dir dir, cc::byte* first_write);
```

- **`curr`, `end`** — the current window, updated **in place**.
  `[curr, end)` is the bytes ready to read (read streams), or the free space to write into (write streams).
- **`ctx`** — the `this` you passed to the stream constructor; `static_cast` it back to your adapter.
- **`dir` + `offset`** — the requested operation:

  | `dir`          | meaning                                   | mutates buffer? |
  |----------------|-------------------------------------------|-----------------|
  | `relative`, 0  | **plain flush**: refill / write-through   | yes             |
  | `begin`        | seek to `offset` from the start           | yes             |
  | `relative`     | seek `offset` from the current position   | yes             |
  | `end`          | seek to `offset` from the end (`<=0` in)  | yes             |
  | `dry_begin/relative/end` | **report** the resulting position | **no**          |
  | `remaining_size_hint` | **report** bytes still to come after `curr` | **no** |

- **Return** the global position of `curr` after the op, `-1` when the source has no meaningful position or is not seekable, or a `cc::result` error on I/O failure.
  `remaining_size_hint` is the one exception: it returns a **count**, not a position.
- **End-of-data rule:** a stream is at its end iff **`curr == end` after a flush**.
  Buffered adapters start with `curr == end` (empty), so the first read triggers a refill; an unbuffered span hands out the full window up front.
- **`dry_*` must not touch** `curr`/`end`/`write_end` or the buffer — they only compute.
  `dry_relative, 0` doubles as the seekability probe (`try_as_seekable`): return `>= 0` if seekable, `-1` if not.
- **`remaining_size_hint` is a size question, not a seek question**, and answering it says nothing about seekability.
  It is what `read_all()` sizes its allocation from, so a source that knows its own length can save the caller a growing buffer without pretending it can be repositioned.
  A decompressing stream reading a frame header is the case it exists for.
  Return `-1` when you do not know, which is always a safe answer; the caller just grows instead.
  Treat it as a hint on the way in as well: it may be stale by the time it is used, so `read_all` is written to survive an answer that turns out too small or too large.
- **Unsupported ops:** the stream never issues a real *seek* outside its capability, and a read-only stream never asks for write-through, so you may `CC_ASSERT` on those.
  **The `dry_*` dirs and `remaining_size_hint` are the exception, and they reach every stream**: `read_all()` and `try_as_seekable()` probe even a non-seekable one.
  Return `-1` from them rather than asserting — that is exactly how a source reports "no meaningful position" or "no idea how much is left".

### Buffered adapters (refill)

When your buffer is smaller than the source — a file, a socket — the plain flush (`relative, 0`) must:

1. Preserve the unconsumed leftover `[curr, end)`, `memmove`ing it to the buffer start, since the ranges overlap.
2. Fill the remainder from the source.
3. Set `curr = base`, `end = base + leftover + bytes_read`.
4. Track the absolute offset of `buffer[0]` yourself, e.g. in a `_buffer_offset` member, advancing it by the *consumed* count, so you can return the global position.
   `curr == end` after this means genuine end-of-data.

`cc::file_read_stream_adapter` in [file_stream.cc](../src/clean-core/streams/file_stream.cc) is the reference implementation.

### Writing: `first_write` and write-through

On a write-capable stream, the first write into the window sets `first_write` to where it began, and the bytes `[first_write, curr)` are *pending*.
On flush you must write them through to the sink.
**Do not reset `first_write`** — the stream clears it after a successful non-dry flush, and leaves it intact on error so the write can be retried.

### Growing adapters (an owned buffer that is also the window)

A third shape sits between the two: the sink is in memory but its size is not known up front.
`cc::vector_write_stream_adapter` / `cc::string_write_stream_adapter` ([growing_stream.hh](../src/clean-core/streams/growing_stream.hh)) are that case.

The trick is that **the window is the container's spare capacity**, so nothing is ever copied on a flush:

```
[data(), data() + size())              already committed
[data() + size(), data() + capacity)   the stream's [curr, end) window
```

The pending bytes `[first_write, curr)` are therefore already stored where they belong, and the plain flush only has to:

1. commit them — `resize_to_uninitialized(max(size(), curr - data()))`, the `max` covering a cursor that was seeked backwards;
2. `reserve_back(...)` for more room, which is what makes the growth exponential and the per-byte cost amortized O(1);
3. recompute `curr` and `end` from the container's (possibly new) base, since the reserve may have reallocated.

Step 3 is the one that bites: a growing adapter is the case where `curr`/`end` being **by-reference** parameters actually matters.

### `read_write` and the second end

A `read_write` stream needs two boundaries at once, so it carries a **`write_end`** distinct from `end`: `end` is the read boundary (end of valid data) and `write_end` is the write capacity.
Set both in your flush.
On every non-`read_write` stream `write_end` *aliases* `end`, so single-capability adapters ignore it.
Keeping them separate is what lets a read_write stream append at EOF: `end == curr` but `write_end > curr`, so there is free write space.
See `cc::file_read_write_stream_adapter`.

## Handing out the stream

Give your adapter a `stream()` method, as above.
For ergonomics, add an implicit conversion so the adapter can be passed straight to a function taking a stream — and, via the `cc::impl::stream_narrows_to` constraint, to any legal *narrowing* too.
A read_write adapter into a plain `cc::read_stream` parameter, for instance.
Construct the target type **directly** from your bounds rather than building the natural stream and narrowing it: same constraint, one step less.

```cpp
template <class Stream>
    requires cc::impl::stream_narrows_to<cc::seekable_read_stream /*natural*/, Stream>
operator Stream() { return Stream(_base, _base + _size, &flush, this); }
```

## Checklist

- The adapter **owns the buffer**; the stream only views it.
  The adapter must outlive any stream taken from it, and — if the buffer is an inline member — must not be moved once a stream is live.
- Implement every `seek_dir` case; you may assert on real seeks your source cannot do, but never on `dry_*` or `remaining_size_hint`.
- `curr == end` after a flush is the only end-of-data signal; get it right.
- `dry_*` and `remaining_size_hint` never mutate.
  Seeks that reposition must leave the window consistent, so buffered adapters refill after seeking.
- Return the global position, or `-1`, and surface I/O failures as `cc::error(...)`.
- Never reset `first_write` yourself, and set both `end` and `write_end` on a read_write adapter.
