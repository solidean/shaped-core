---
created: 2026-11-06
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
  - file: libs/graphics/shaped-graphics-language/docs/comparison/matrix.md
---

# GPU pointers: whether SGL has them, and what shape

The design question behind matrix row D4, which is ❌ today and the one SGL row with no plan.
Nothing here is decided, and "no pointers, by design" is a legitimate outcome, as "no interfaces, by design" is for D1.
[feat-sgl-gpu-pointers](feat-sgl-gpu-pointers.md) is the first child: the uses that bindings do not serve.

## What the targets have

| Target | What it offers | Cost of reaching it |
|---|---|---|
| vulkan | buffer device addresses (`VK_KHR_buffer_device_address`, core in 1.2), through DXC's `vk::RawBufferLoad` / `vk::BufferPointer` | DXC's SPIR-V arm only; the Arc B580 here has it |
| metal | `device T*` everywhere, and `gpuAddress` on a buffer | native; testable on the Mac alone |
| dx12 | none: DXIL has no pointer into GPU memory | — a target limit, unless emulated |
| webgpu | none, and no proposal close to landing | — a target limit, unless emulated |

So a native pointer type reaches two of four targets, which makes it a feature dx12 and webgpu lack, as `ray_query` is for webgpu.
A row whose best answer leaves dx12 and webgpu out can still be ✅ (see the matrix legend), so that is not disqualifying by itself.

## What a design has to settle

- **The uses.** What a pointer buys that a binding array of buffers or `bytes` at an offset does not.
  Candidates: per-instance geometry reached from a hit, linked structures, a layout the host decides at run time, and porting code written against BDA.
  If every named use is served by a binding array plus an index, the epic closes with "no".
- **Native or emulated.** An emulation as a pair of a binding-array index and a byte offset would reach all four targets, dx12 and webgpu included.
  That is a fat pointer over `bytes[]`, which costs a register and a bounds story, and is only as available as binding arrays are (none on webgpu).
  It may be the honest portable form, with the native pointer as the fast path where a target has one.
- **Typing.** A pointer to a typed element (`ptr[Particle]`) or raw bytes (`ptr[bytes]`).
  Layout follows the existing GPU-memory layout rules, so `mat3` and nested arrays inherit their open questions.
  Whether pointer arithmetic exists, or only an index on a typed pointer.
- **Access and aliasing.** Read-only vs mutable pointers, as `bytes` vs `mut bytes`.
  What the footprint (C2) reports for memory reached through a pointer, which is no slot.
  Coherence and atomics through a pointer.
- **Where a pointer comes from.** A host-supplied `ulong` in a constant buffer or a buffer element, and how sg hands the address of a buffer out (sg has no API for it today).
  Lifetime: a pointer into a buffer the host destroyed is the host's error, which nothing on the GPU can see.
- **Robustness profiles.** A pointer has no `values.length`, so LEGAL-66's guard cannot apply.
  Either a pointer carries its length (fat), or `report` and `define` say plainly that pointer accesses are outside them.
- **Interpreter.** EVAL needs a memory model for pointers, so a `test` can use one and an out-of-range one is a program error there.
- **Host code.** What slib generates on the C++ side for a struct that holds a pointer.

## Prior art

Slang's pointers and `DescriptorHandle`, DXC's `vk::BufferPointer` proposal (HLSL 0010), MSL's address spaces, and CUDA's.
GLSL's `GL_EXT_buffer_reference`, and how SPIRV-Cross lowers it to MSL.

Each piece becomes a child issue when it is taken up.
