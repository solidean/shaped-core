---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
---

# What a compute entry point still cannot lower

A body or a type in a target's own text, and an `affine3` in GPU memory.

## Left, and why

- **A body in a target's own text** (`double_values.sgl`'s `triple_values`) and **a type spelled in it** (`repack_values`'s `packed`, CHK-432), on both writers.
  These stay text by what they are: the source states the target's text, which only that target's compiler reads.
  What the prelude says that way is another matter: `maybe_reorder` moved to SGL, and so may anything else of the prelude's.
- **Metal's** fused intersection and empty closest hit, which no other target builds (EMIT-244).

## Landed on 2026-11-07

| gap | rule | where |
|---|---|---|
| 64-bit atomics on DXIL, on a buffer and in workgroup memory | EMIT-247 | the DXIL writer |
| EMIT-229's workgroup prologue, under every robustness profile | EMIT-248, LEGAL-71 | legalize |
| DXIL's shuffles, ballot, quad reads and inclusive folds | EMIT-249 | the DXIL writer |
| a float atomic with no device add | EMIT-250 | shared lowering |
| `bool32` | EMIT-251 | shared lowering |
| texel atomics, in every image shape | EMIT-252 | `op::texel_pointer`, and a texel place on DXIL |
| a `sample` or a `gather` offset | EMIT-253 | both writers |

The prologue is written once: legalize writes it as statements over the shadow's words (LEGAL-71), and every route lowers them.

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

## Texel atomics, measured on 2026-11-07

`op::texel_pointer` is the image and the coordinate as a place, a pointer of `storage::image` every atomic takes unchanged.
A texel load or store could take the same pointer, though nothing lowers one through it yet.
Against DXC's output for `reduce_max`, the operations are the same: `OpImageTexelPointer` then `OpAtomicUMax` and `OpAtomicIAdd`, and `atomicBinOp` 7 and 0 with `undef` past the coordinate.

| | before | after |
|---|---|---|
| SPIR-V, compute | 117 of 120 | 119 of 121 |
| DXIL, compute | 116 of 120 | 118 of 121 |
| SPIR-V, under the robustness profiles | 876 | 882 |
| DXIL, under the robustness profiles | 849 | 855 |
| both writers, with the device features | 687 | 691 |

The 121st entry is `coherence.sgl`'s new `texel_shapes`, one of each image shape.
It takes a signed `min` on a 1D image, an `add` on a 2D array's layer, an `exchange` on a 3D image, both compare-exchanges, and an atomic store and load.
Its GPU test, in `compute-coherence-test.cc`, checks what each returns and leaves on every context, and a layer and a depth of their own beside each.
A layer lowered as 0 failed `dx12-native-dxil` and `vulkan-native-spirv` alone, and DXC's contexts passed.

## A sample's offset, measured on 2026-11-07

The offset is a `sample` or a `gather` operand of its own, the `construct` of constants CHK-280 makes it, so the form holds no constant vector.
SPIR-V declares it as an `OpConstantComposite` where an image operation names it, and DXIL reads its lanes as immediates.
`pixel_surface.sgl`'s new `offset_ps` takes both, and the mixed-route test draws it through every pairing on dx12 and vulkan, every lane within 1e-4 of DXC's.
The vertex and pixel sweep lowers 83 of 83 on both writers, and dropping the offset in lowering failed every native pixel pairing.

## A double's maths, measured on 2026-11-07

SGL gained what the helpers needed: `fma` of every float type (EVAL-146, EMIT-254), and `d.bits` as a `uint2` with `double.from_bits` (CHK-517, EMIT-255).
The helpers are SGL in `core.sgl` now, and EMIT-219's HLSL is gone; each body chooses by `target_api` what it lowers to (EMIT-256).

| route | `sqrt` and the roundings, before | after |
|---|---|---|
| `hlsl-dx12` | the HLSL helpers | the SGL, inlined |
| native DXIL | a gap, so DXC under a preference | the SGL |
| `hlsl-vulkan` | the HLSL helpers | GLSL.std.450's, through DXC's inline SPIR-V |
| native SPIR-V | GLSL.std.450 `Sqrt`, `Floor` and the rest | GLSL.std.450's |

For a while both vulkan routes took the SGL too, on the reading that a body could not tell native SPIR-V from DXC's text.
`target_api` tells the APIs apart, and the API is where the instructions differ, so vulkan went back to GLSL.std.450 on both its routes.
The bodies were no sounder there: Vulkan states a double's precision as at least a float's for every instruction, the bodies' own `fma` included, which it states as a multiply then an add.
DXC hands a double to its own `floor` or `sqrt` as a float even for SPIR-V, so its text declares each instruction through inline SPIR-V instead.

On the Arc B580 both vulkan routes now agree bit for bit, and its driver's double `Sqrt` and `InverseSqrt` came out IEEE's to the bit over every positive normal test value.
Its zeros, negatives, infinities and NaN came out IEEE's too, which no spec promises there.
dx12's bodies stayed within 0 and 2 ULP.

The interpreter runs the same bodies, so the roundings are bit for bit what the GPU gives.
`sqrt`, `inverse_sqrt` and `smoothstep` take its own answer where the language is `.sgl`: the nearest root, which EVAL-108 promises, and smoothstep's missing value.
That missing value now stands at the program's call rather than inside `core.sgl` (CHK-311).

On the Arc B580, with the Agility SDK, `--thorough`:

| | before | after |
|---|---|---|
| DXIL, compute | 118 of 121 | 120 of 122 |
| SPIR-V, compute | 119 of 121 | 120 of 122 |
| DXIL, under the robustness profiles | 858 | 864 |
| SPIR-V, under the robustness profiles | 885 | 888 |
| both writers, with the device features | 691 | 696, none refused |

The 122nd compute entry point is `sgl_mixed/double_routes.sgl`'s `double_steps`, and the two DXIL leaves are the target-text pair above.

`compute-double-maths-test.cc` holds two tests:

- The roundings exact and `sqrt` within an ULP, over 256 values with every edge case: both zeros, subnormals, ties, integers past 2^52, infinities and NaN.
  It passes on all four contexts, `dx12-native-dxil` now through SGL's own DXIL.
- `double_steps` through DXC and through the native route in one context, every lane alike to the bit, on `dx12-hw` and `vulkan`.
  On dx12 it also holds that `fma((1 + 2^-27), (1 + 2^-27), -(1 + 2^-27))` is `2^-27 + 2^-54`, which a product rounded first loses.
  Swapping the two words DXIL's `splitDouble` hands back failed it with 30 checks, which the other contexts passed; a double `fma` written as `fmad` is refused at pipeline creation.

Not confirmed: whether vulkan's `Fma` of a double rounds once on the B580, since the test accepts either answer there.
