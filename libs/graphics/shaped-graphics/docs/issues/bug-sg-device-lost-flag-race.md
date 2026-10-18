---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/context.hh
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/context.cc
  - symbol: sg::context::mark_device_lost
  - symbol: sg::context::is_device_lost
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-18T05:58Z
---

# `context::_device_lost` is a plain bool, and metal writes it from a driver thread

Every other piece of sticky context state is guarded or atomic.
This one is a bare `bool` set by `mark_device_lost` and read by `is_device_lost` on any thread.
It was sound while no backend wrote it from a thread sg does not own.

Metal is the first that does: its only error channel is the `MTL4CommitFeedback` handler, which runs on a dispatch queue Apple owns.
That handler calls `report_feedback_error` and so `mark_device_lost`.
dx12 polls `GetDeviceRemovedReason` from the calling thread and vulkan's debug callback normally arrives on it too, so neither backend exposed this.

The race is benign in practice — a sticky flag written once with one value.
It is still a data race a sanitizer is entitled to report, on a field every backend reads per frame.

The fix is `cc::atomic<bool>` plus the release/acquire pair, which costs nothing on the read path.
It was filed as a question rather than a patch because `SC_THREADS=OFF` turns `cc::atomic` back into a plain value, and a driver thread does not go away with that flag.
So it might want the same treatment metal's own callback state got.
Since then `SC_THREADS=OFF` is refused on Apple targets ([platforms](../../../../../docs/platforms.md#threading-sc_threads)).
So wherever metal runs, `cc::atomic` is a real atomic, and the patch may be all it needs.
