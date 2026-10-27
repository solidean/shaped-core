---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/container/pinned_data.hh
  - symbol: cc::pinned_data::create_from_pin
  - symbol: babel::gltf::read
---

# A memory-mapped file behind a `cc::pinned_data`

Wanted: an mmap-backed pin — a `cc::pinned_data<byte const>` over a mapped file, with the unmap living in the pin's deleter.

It is the piece that would make babel's zero-copy story end to end.
`babel::gltf::read` already returns every embedded buffer as a subview of its input pin, so a 200 MB `.glb` never copies its vertex data inside babel.
Getting those bytes in still costs one full read into memory, because a file is only reachable as a stream today.
With a mapped pin, loading a glTF would touch the vertex bytes only when the caller reads them.

Nothing in the repo maps a file (no `mmap` / `MapViewOfFile` wrapper anywhere).
So the composition is `file_read_stream_adapter::open` → `read_all()` → `cc::make_pinned_data(cc::move(bytes))`: one precise allocation and one copy.
The seam is already the right shape: `pinned_data::create_from_pin(span, std::shared_ptr<void const>)` accepts any deleter, so a mapped pin drops in without touching a babel signature.
