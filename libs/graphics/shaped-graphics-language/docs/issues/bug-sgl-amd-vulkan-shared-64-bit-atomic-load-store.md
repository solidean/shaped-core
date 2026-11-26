---
created: 2026-11-07
refs:
  - file: docs/bugs-external/amd-vulkan-shared-64-bit-atomic-store-keeps-high-word/readme.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_operations.cc
  - test: sg vulkan - workgroup 64-bit atomics on a device that has them (CHK-510)
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-26T06:57Z
---

# A 64-bit atomic load or store of workgroup memory reads wrong on AMD's vulkan driver, so SGL writes DXC's exchange and or instead

Found on 2026-11-07 when the native route became the default, and first written down as the Arc B580's.
It was not: the Arc has no `shaderSharedInt64Atomics`, and the test picks the first vulkan device that has it, the AMD Radeon iGPU beside it.
`sixty_four_bit_shared.sgl`'s `shared_sums` adds 2^33 plus its index from each of 64 threads, and reads back only the low 32 bits: 2016 where 2^39 + 2016 is right.
That test builds its own context, so until the default changed it only ever ran DXC's SPIR-V.

SGL's SPIR-V passes `spirv-val`, and differs from DXC's in three ways, which were taken one at a time:

| store | load | scope | result |
|---|---|---|---|
| `OpAtomicStore` | `OpAtomicLoad` | `Device` | wrong |
| `OpAtomicStore` | `OpAtomicLoad` | `Workgroup` | wrong |
| `OpAtomicExchange` | `OpAtomicLoad` | `Workgroup` | wrong |
| `OpAtomicStore` | `OpAtomicOr` of 0 | `Workgroup` | wrong |
| `OpAtomicExchange` | `OpAtomicOr` of 0 | `Workgroup` | right, as DXC's |

So a 64-bit atomic load and a 64-bit atomic store of `Workgroup` memory each gave a wrong result on this driver, and the scope changed nothing.
EMIT-258 writes DXC's shape for them; `Workgroup` scope stays, since it is the right one for workgroup memory.
A 64-bit atomic load or store of storage-buffer memory was not checked on this driver; the buffer tests that pass use other operations.

## Log

**2026-11-26: reproduced standalone, on AMD's driver.**
[The standalone repro](../../../../../docs/bugs-external/amd-vulkan-shared-64-bit-atomic-store-keeps-high-word/readme.md) runs the table's five rows as hand-written SPIR-V.
On vulkan driver 2.0.353 only the plain store followed by an atomic `or` reads wrong, on every run: the low word right, the high word what the memory held before.
The rows with an atomic load read right there, unlike the table above.
Whether the driver changed since, or the shader's second variable is part of the trigger, is open.
The workaround stays, since its shape is the one row right on both.
