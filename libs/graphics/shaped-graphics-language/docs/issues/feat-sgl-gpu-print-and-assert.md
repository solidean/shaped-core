---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/shader-logging.md
  - file: libs/graphics/shaped-graphics-language/readme.md
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-02T15:48Z
---

# `print` and `assert` on the GPU

`print` and `assert` run on the interpreter only; `legalize` removes them from every target's text.
[incubator/shader-logging.md](../spec/incubator/shader-logging.md) sketches how they would reach a GPU, and nothing of it is built.

A portable form needs no target support: a buffer the host reads back, written through an atomic cursor, on all four targets.
The readme lists "assertions, logging, per expression traces" among what SGL offers, which holds for the interpreter alone until this lands.

## Design (agreed 2026-11-02)

- **Buffer.** sg owns one debug buffer per context, a ring per epoch, in its reserved group: a root UAV on dx12, the reserved set on vulkan, `@group(3)` on webgpu, an argument-table slot on metal.
  It is never one of the user's bindings or part of the footprint; sg places its one barrier, before the readback.
- **Layout.** A header `{cursor, capacity, dropped}`, then u32 words; a record is `{site, word count, payload…}`, reserved with one atomic add.
  A full buffer counts the record in `dropped` and writes nothing, and the host reports `N more dropped` per site.
- **Site table.** The shader writes only the site id and the interpolated values.
  The string pieces, payload types, file and line with the inlined chain, and an assert's condition nodes are metadata `describe` emits.
  So a failure reads like the interpreter's: `a < b failed: a = 3, b = 2`.
  A site id is a 32-bit hash of module, entry point and site; slib registers the tables as it acquires shaders and detects a collision there.
- **Failure.** A failing assert records, then ends the invocation: compute and ray-tracing stages return, a pixel shader discards, a vertex shader writes a NaN position.
- **Host.** sg drains each completed epoch into `cc::rec`: `print` is info and a failed `assert` an error, so nexus's log rule fails any test that trips one.
- **Gate.** The build knob `SC_SHADER_DIAGNOSTICS`, its default following `SC_FROZEN_SHADERS` (on unless frozen) but set on its own.
  Off, both are removed, and a `print` no longer makes an entry point `unsupported` (EMIT-67).
- **Layering.** sgl stays on clean-core alone; sg gains the site-table vocabulary on `compiled_shader`; slib translates `describe` into it, as it does the footprint.
- **Not in v1.** Per-site hit counters, struct and matrix payloads, and the incubator's implicit asserts for program errors.
  webgpu's vertex stage has no writable storage, so a `print` or `assert` there drops with one warning.

## Stages (2026-11-02)

1. Done: interpolated messages on the interpreter (CHK-424, EVAL-103).
2. Done: the compiler writes records with diagnostics on (LEGAL-63, LEGAL-64, EMIT-183); without them the text holds neither.
   MSL takes the buffer at `[[buffer(30)]]`, since `[[buffer(6)]]` is a vertex buffer's slot.
   A failed assert in a vertex, geometry or tessellation stage, an any hit or an intersection records and goes on, since ending one means writing a result.
3. sg, decided: every pipeline layout gets the debug slot when diagnostics are on, and layout-fit and slib's WGSL reader accept `sg_debug` by name, as they do the acceleration pool.
   dx12 binds it as a root UAV after the inline constants, set where each of the three root signatures is set.
   webgpu adds group 3's binding 19, its bind group keyed by the ring slot as it is by the pool's generation.
   vulkan's set 3 is an embedded-sampler set, which holds samplers only.
   With diagnostics it becomes a descriptor-buffer set of the samplers and binding 19, one copy per ring slot, bound with `cmd_set_descriptor_offsets`.
   metal is Julius's: `[[buffer(30)]]`.
   Each epoch writes its own slice of a per-context ring, read back once the epoch completes and logged in the `sg` domain.
4. slib and the build: the site table translated onto `sg::compiled_shader`, `SC_SHADER_DIAGNOSTICS`, and GPU tests.
