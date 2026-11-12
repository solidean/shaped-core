---
created: 2026-11-09
priority: next
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
closed: 2026-11-09
resolution: fixed
---

# A bare @compute chooses its own workgroup size where nothing in it depends on the group, and a size that wastes a wave warns

Most compute kernels are image or buffer passes whose workgroup size is no algorithmic choice, and an author picks one by habit.
A bare `@compute` should choose a good one, so nobody has to think about it and a kernel whose indexing changes adapts by itself.
*Decided with the maintainer on 2026-11-09:* useful in general, built after B10's robustness work; this tree's kernels are sized by experts and are not the case it serves.

## The design

- **A bare `@compute` is `auto`**, legal where nothing the entry point reaches depends on the group's shape.
  That is no barrier, no `@workgroup` binding, no subgroup operation, and neither `@local_thread_index`, `@local_thread_id` nor `@workgroup_id`.
  Otherwise it is an error naming the construct, and the kernel states its size, `@compute(x, y, z)`, as today.
- **An `auto` entry point is dispatched by threads only**, which is already sg's recommended dispatch; dispatching it by groups is refused, since the group count is not the author's to know.
  The compiler writes the early return of a thread past the grid, against the grid size sg passes in.
- **The size follows the indexing**: 64 where the body reads `id.x` alone, 8×8 where it reads `id.xy`, 4×4×4 where it reads all three.
  Every one is a multiple of 64, which fills waves of 32 and 64 and Intel's 8- and 16-lane modes alike.
- **Tuning is a later opt-in**: a workgroup size may name an `@option` already (CHK-370), so slib can compile a few candidates.
  It times them once per device with sg's timestamps, and keeps the fastest in blob-cache.
  Nothing observable depends on the size once the legality rule holds, so a tuned size changes no result.

## Warnings on a stated size

- A total below 32 that is not 1: less than a wave on every GPU.
- A total that is not a multiple of 32: a partial wave in every group.
- A total above 256, or a dimension past WebGPU's 256, 256 and 64: no webgpu device runs it; a webgpu compile refuses it already.
- A group shaped against the body's indexing: a 1D group over `id.xy`, or a 2D group where only `id.x` is read.
- A size where `auto` would be legal is no warning: stating one stays a legitimate choice.

*As built:* the warnings judge only an entry point `auto` would be legal for, whose size is habit rather than its algorithm's.
The tests' group-feature kernels run 4 or 16 threads on purpose, and a warning on them would only be declared away.
A 1D group over `id.xy` is no warning: a row of 64 over a 2D grid is coalesced, and fired on fixtures for no waste.
A body that reads no thread id says nothing of its shape either.
`@subgroup_size` and `@subgroup_invocation_id` count as group inputs too, beside the local id, the local index and the group id.
Tuning and the open questions moved to `feat-sgl-compute-auto-tuning`.

## Measured on 2026-11-09

All 59 production compute entry points of the tree would be legal as `auto`: no production shader or module uses a barrier, workgroup memory, a subgroup operation, the local index or the group id.
They state what the default would choose: 34 `(8, 8)`, 18 `(64)`, one `(4, 4, 4)`, and six `(1)` serial kernels, which keep their size.
Of the tests' 194, 173 would be legal; the rest test the group features themselves.

## Open

- Whether a serial kernel, `@compute(1)` dispatched as one thread, wants a spelling of its own rather than a stated size.
- What tuning keys on: the device alone, or the device and the grid's size.
