---
created: 2026-11-27
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.queries.cc:38
  - test: "sv - the wavefront tracer traces and denoises the viewer's image"
---

# A GPU timestamp query once hung inside Intel's dx12 driver, under a full ship gate

On 2026-11-27 the nexus watchdog stopped shaped-viewer-test during a `dev.py ship` gate, on an Arc B580, driver 32.0.101.9150, system runtime.
The run was awaiting `sv - the wavefront tracer traces and denoises the viewer's image` on `sv dx12 - hardware`, and nothing else was running.
The one busy thread was inside `igd12um64xe2.dll`, under D3D12's debug layer, from `dx12_command_list::query_record_gpu_timestamp` (`EndQuery`).
That list was recorded by `sv::wavefront_routine::execute`, from `denoise-view-test.cc:179`.
Its top frames are the driver's own code rather than a wait, so it was likely spinning; one snapshot cannot tell a spin from slow progress.
The rerun the gate made passed, and four repeats of the whole viewer suite straight after passed too.

Measured: the stack above, in the crash folder's report (`shaped-viewer-test-20261127-150831-38500`).
Not known: whether the query heap or the list was in any unusual state, and whether it repeats.
A second occurrence with the same stack is what would make it a bugs-external entry.
