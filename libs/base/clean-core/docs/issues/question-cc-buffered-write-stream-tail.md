---
created: 2026-10-17
tags: [streams]
refs:
  - file: libs/base/clean-core/src/clean-core/streams/file_stream.hh
  - symbol: cc::file_write_stream_adapter
  - symbol: cc::rec::save_serialized_recording
---

# What should happen to a buffered write stream's unflushed tail?

Nothing auto-flushes today, so bytes still in the window when the adapter dies are simply lost, and the file ends on a `k_buffer_size` boundary.
Every whole-file caller pays a `flush()` for it, and `nexus::run` even carries a comment saying why.
`cc::rec::save_serialized_recording` sidesteps the adapter entirely, writing through `native_file` in a hand-rolled loop.

The first caller to forget it lost the tail of a JPEG, which still decoded.
A truncated scan is flat-filled from the last value read, so it read as a bug in whatever produced the pixels.
**A silent failure whose symptom points somewhere else is the thing to fix**, and the fix is not obvious.

- A destructor cannot do it: the write position lives in the stream, and the adapter holds only the buffer, so it has nothing to flush with.
- Moving it into the stream's destructor puts a non-trivial destructor on a type built so that every operation inlines.
  It also swallows the one error — a full disk — that must not be swallowed.
- A whole-file helper was considered and rejected: too easy to get wrong, and it does not fit the filesystem abstraction this is heading toward.

So this is recorded as an open question rather than a plan.
