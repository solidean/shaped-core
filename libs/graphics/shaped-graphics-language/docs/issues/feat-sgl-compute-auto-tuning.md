---
created: 2026-11-09
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
---

# A bare @compute's size is tuned per device among a few candidates, timed once and kept in blob-cache

CHK-520 sizes a bare `@compute` by the axes of its thread id: 64, 8 × 8 or 4 × 4 × 4.
That is a good default and no measurement; this is the opt-in that measures.

## The idea

- A workgroup size may name an `@option` already (CHK-370), so slib can compile a few candidates of one entry point.
- It times them once per device with sg's timestamps, and keeps the fastest in blob-cache.
- Nothing observable depends on the size once CHK-520's legality holds, and the grid guard makes the tail threads return, so a tuned size changes no result.

## Open

- What tuning keys on: the device alone, or the device and the grid's size.
- Whether a serial kernel, `@compute(1)` dispatched as one thread, wants a spelling of its own rather than a stated size.
