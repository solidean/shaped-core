---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer.cc
  - file: libs/graphics/shaped-viewer/shaders/hybrid_pass.sgl
  - test: "sv - a frame mesh traces in the frame it is placed, and keeps the image only while its content holds"
---

# Native DXIL's hybrid lighting hangs Intel's dx12 device unless neighboring buffer loads are merged

With native DXIL the default, `shaped-viewer-test` hung the dx12 device (`DXGI_ERROR_DEVICE_HUNG`) on the Arc B580, driver 32.0.101.9150, Agility 1.620.1.
It no longer does on main, but only because `merge_buffer_loads` runs: the hang is hidden, not understood.

## Measured

- **Bisected to one pass.** The flip on top of `1810c768b` hangs; on top of `9c0de9116` it passes.
  Between them only `52f047d3d` changes code, which made a `bytes` address the byte offset and added `merge_buffer_loads`.
- **The address form does not matter; the merging does.** Main with `merge_buffer_loads` skipped in `writer.cc` hangs, three runs of three.
  The old address, `(bytes >> 2) * 4`, is the new one, `bytes`, modulo 2^32 for every offset EVAL-97 allows.
- **One test reproduces it in 7 s**, with the merging skipped: `uv run dev.py test "sv - a frame mesh traces in the frame it is placed"`.
- **DRED names a hybrid frame.** Raster draws, then a `Dispatch`, then an operation after it that never completes.
  The hybrid routine's `light_thin` (`hybrid_pass.sgl`) is the one compute dispatch that follows the G-buffer's draws.
- **The kernel is large.** `light_thin`'s native DXIL is 43,431 operations and 1,827 `rawBufferLoad`s unmerged, against DXC's 36,094 and 1,321; it passes DXC's validator either way.
- The `@unroll` of `f5ead615c` is not involved: main without it passes.

## Not known

- Whether this is our bug or Intel's compiler.
  Merging changes no value a load reads, so the unmerged module means the same thing; a miscompile in `igc` under this kernel's size is the leading guess, unconfirmed.
- Which loop, if any, fails to exit.
  The next step is a reduction: cut `light_thin` down until the unmerged form stops hanging, then compare the two listings of what remains.
- Measured since: the AMD Radeon iGPU (RDNA 2, driver 32.0.21042.62) does not hang on the unmerged form.
  With the merging skipped, `SC_GPU=amd SC_SG_COLD=all uv run dev.py test "sv - a frame mesh traces in the frame it is placed"` passed on its dx12 context in 7.8 s.
  The same module hangs Intel's, which makes Intel's compiler the likelier culprit; a reduction is still what would settle it.

## The risk left open

A shader whose loads `merge_buffer_loads` cannot gather, and that grows like `light_thin`, may hang the device the same way, now that native is the default.
