---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_operations.cc
  - test: sg vulkan - workgroup 64-bit atomics on a device that has them (CHK-510)
---

# A 64-bit atomic load or store of workgroup memory reads wrong on Intel's vulkan driver, so SGL writes DXC's exchange and or instead

What is wrong or wanted, and what is already known.

Found on 2026-11-07 when the native route became the default, on the Arc B580 (driver 32.0.101.9150, vulkan).
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

So a 64-bit atomic load and a 64-bit atomic store of `Workgroup` memory each give a wrong result on this driver, and the scope changes nothing.
EMIT-258 writes DXC's shape for them; `Workgroup` scope stays, since it is the right one for workgroup memory.

Not done, which is why this is not under docs/bugs-external yet: a standalone repro with no code of ours, and any other vendor's driver.
A 64-bit atomic load or store of storage-buffer memory was not checked on this driver either; the buffer tests that pass use other operations.
