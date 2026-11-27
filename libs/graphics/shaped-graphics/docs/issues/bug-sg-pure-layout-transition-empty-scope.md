---
created: 2026-10-17
parent: epic-sg-transfer-gaps
tags: [vulkan]
refs:
  - file: libs/graphics/shaped-graphics/tests/transfer/texture-async-interleave-test.cc
  - file: libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.cc
  - symbol: sg::access_barrier
---

# A pure layout transition is modelled as touching nothing, so nothing orders against it

`cmd.ensure_layout` — and the async fixup, which is one — declares no stage and no access, since it asks for a layout and nothing else.
The barrier that produces therefore has an empty scope on both sides, and two things follow from that.

- Its destination scope orders nothing after it within its own submit.
- The state it commits records no write, so the *next* list's entry barrier is computed against a timeline that has forgotten the transition happened.

Synchronization validation reports the second one as a `READ_AFTER_WRITE` against "a prior layout transition".

**The missing gate case is the forward edge read through an inline readback**: an async upload, then a list recorded straight afterwards, on a texture the fixup transitioned.
Its bytes are right on both backends — the semaphores order it correctly — and only the layer disagrees.
[texture-async-interleave-test.cc](../../tests/transfer/texture-async-interleave-test.cc) carries the rest of the gate and would carry this one too.

**The fix is to model a pure transition as a write at full scope**, which a transition physically is.
Neither `pipeline_stage_flags` nor `access_flags` spells "all", and naming every stage is not the answer either.
The vulkan initial-transition prepend deliberately avoids that, since a ray-tracing stage is invalid on a device without the extension.
So it wants a marker on `access_barrier` and on the in-flight state — "full scope, widen at emission".
Each backend already knows how to spell that: `ALL_COMMANDS` plus `MEMORY_READ | MEMORY_WRITE` on vulkan.

**The vulkan present path pays for this today, and the workaround is a wait mask.**
A presenting submit waits the acquire semaphore at `ALL_COMMANDS` rather than at `COLOR_ATTACHMENT_OUTPUT`, the stage that actually writes the back buffer.
A wait dst stage orders that stage and later ones, and the back buffer's entry transition runs ahead of every stage in the prepended buffer.
So the narrower mask left the transition unordered against the acquire — a `WRITE_AFTER_READ` against `vkAcquireNextImageKHR`.
Once a transition carries a full scope of its own, the mask can go back to naming the stage the work is in.
