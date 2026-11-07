---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# What a compute entry point still cannot lower

Texel atomics, a body or a type in a target's own text, DXIL's `double` maths, and an `affine3` in GPU memory.
A sample's offset too, which no fixture holds.

## Left, and why

- **Texel atomics** (`coherence.sgl`'s `reduce_max`), on both writers.
  A texel is an image and a coordinate rather than a place, so `lowering::atomic` has no pointer to update.
  It wants a place of its own: `OpImageTexelPointer` on SPIR-V, whose pointer the atomics already take, and on DXIL the image's handle with the coordinate as `atomicBinOp`'s three offsets.
- **A body in a target's own text** (`double_values.sgl`'s `triple_values`) and **a type spelled in it** (`repack_values`'s `packed`, CHK-432), on both writers.
  These stay text by what they are: the source states the target's text, which only that target's compiler reads.
  What the prelude says that way is another matter: `maybe_reorder` moved to SGL, and so may anything else of the prelude's.
- **DXIL's `double` maths** (`double_maths.sgl`'s `double_functions`): `floor`, `ceil`, `round`, `trunc`, `fract`, `sqrt` and `inverse_sqrt`.
  DXIL has no operation for any of them at 64 bits, so the text writes EMIT-219's helpers of HLSL, about 340 lines that take the bits apart.
  The native route would want the same helpers once, written in SGL in the prelude, rather than a second copy in either writer.
- **A sample's offset**: SPIR-V takes it as a constant vector, which the form does not hold (EMIT-238).
- **Metal's** fused intersection and empty closest hit, which no other target builds (EMIT-244).

## Landed on 2026-11-07

| gap | rule | where |
|---|---|---|
| 64-bit atomics on DXIL, on a buffer and in workgroup memory | EMIT-247 | the DXIL writer |
| EMIT-229's workgroup prologue, under every robustness profile | EMIT-248 | shared lowering |
| DXIL's shuffles, ballot, quad reads and inclusive folds | EMIT-249 | the DXIL writer |
| a float atomic with no device add | EMIT-250 | shared lowering |
| `bool32` | EMIT-251 | shared lowering |

The prologue is a copy: the text route still writes EMIT-229 by hand, and lowering writes it again.
Like `print` before LEGAL-70, it belongs in legalize, as flat statements over a workgroup `sgl_written` every route then lowers.

## Measured on 2026-11-07

On the Arc B580, with the Agility SDK, over the repo's SGL fixtures, `--thorough`:

| | before | after |
|---|---|---|
| DXIL, compute | 109 of 119 | 116 of 120 |
| DXIL, under the three robustness profiles | 813 | 849 |
| SPIR-V, compute | 114 | 117 of 120 |
| SPIR-V, under the robustness profiles | 282, `report` alone | 876, all three |
| both writers, with `opacity_micromap`, `shader_execution_reordering` and `vertex_pipeline_stores` | 677 | 687 |

Every module passes DXC's validator or `spirv-val`, and the 120th compute entry point is `subgroups.sgl`'s new `exchange`.
No robustness profile leaves an entry point to DXC on either writer any more.

Each lowering runs on the GPU through `dx12-native-dxil` and `vulkan-native-spirv`, against values the CPU states:

- 64-bit atomics: `compute-sixty-four-bit-test.cc`'s buffer tests; the workgroup one skips, since the B580 has no 64-bit atomics in workgroup memory.
- The prologue: sr's `debug-output-test.cc`, whose host compiles now take their context's options (EMIT-245).
- The subgroup family: `exchange`, in `compute-sync-test.cc`, at whatever size the subgroups are.
- A float atomic: `compute-float-atomics-test.cc`; `vulkan-native-spirv` takes the loop on the B580 too, and why it holds the float as bits there was not looked into.
- `bool32`: `compute-test.cc`'s layout test.

A temporary mutation of each failed on the native contexts alone: a 64-bit `umin` as `umax`, a shadow cleared to ones, quad axis 1 as 2, the float sum added twice, and `as bool` read as `== 0`.
