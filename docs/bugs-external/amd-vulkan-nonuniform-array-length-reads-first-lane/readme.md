# AMD: OpArrayLength of a binding array's element at a non-uniform index answers every lane with the first lane's element

**Status:** open, reproduced standalone, not yet filed upstream, worked around in SGL; measured on 2026-11-08.
**Affects:** AMD vulkan driver 2.0.353 (`0x00800161`), the Radeon iGPU of a Ryzen 9 9950X3D2 (RDNA 2), Windows 11; descriptor sets and descriptor buffers alike.
**Found by:** `shaped-rendering-test`'s guard tests on that GPU, `SC_GPU=amd`: lanes indexing a binding array's second element measured their index against the first element's length.

## What happens

A shader indexes an array of storage buffers at an index marked `NonUniform`, and asks `OpArrayLength` of the element it reaches.
Every lane of the subgroup gets the length of the element the first active lane indexes.
A load through the same access chain reads the lane's own element, so only the length is wrong.
dx12 on the same GPU answers each lane its own length, and so does the Intel Arc B580 on vulkan.

## The trigger

`length.spvasm` is the whole shader: 4 lanes, lane `x` reading `views[(x + shift) % 2]`, a view of 2 words and a view of 8.
The index, the access chain and the loaded word are all decorated `NonUniform`, and `spirv-val` accepts the module.

| arm | AMD lengths | right lengths | first words, AMD and right |
|---|---|---|---|
| descriptor buffer, `shift` 0 | 2, 2, 2, 2 | 2, 8, 2, 8 | 10, 20, 10, 20 |
| descriptor set, `shift` 0 | 2, 2, 2, 2 | 2, 8, 2, 8 | 10, 20, 10, 20 |
| descriptor set, `shift` 1 | 8, 8, 8, 8 | 8, 2, 8, 2 | 20, 10, 20, 10 |

The `shift` 1 row says it is the first lane's element rather than element 0: every lane gets 8 once the first lane reads the long view.
The descriptor set rows say descriptor buffers are not needed.
The likely mechanism is the length read once, from a scalar register, the way a uniform index allows; that is a guess from the symptom.

## Why it is not our bug

`repro.cc` is the Vulkan loader and the C++ standard library, and the shader is hand-written SPIR-V the validator accepts.
The same module on the same descriptors gives the right lengths on Intel, and the loads on AMD reach the right elements.
DXC's SPIR-V for the equivalent HLSL, `views[NonUniformResourceIndex(i)].GetDimensions(n, stride)`, failed the same way in shaped-core's tests.

## Reproducing it

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

It assembles `length.spvasm` with the SDK's `spirv-as`, builds `repro.cc` with clang-cl against the SDK's headers and `vulkan-1.lib`, and runs the three arms.
`--vulkan-sdk` defaults to `$VULKAN_SDK`, then to the newest `~/tools/vulkan-sdk/<version>`.
`--vendor 1002` picks the AMD device; the first discrete GPU otherwise.
It exits 1 when an arm's loads are right and its lengths wrong, and 0 when every arm is right.
On the machine above, with `--vendor 1002`, it prints:

```
descriptor buffer:
    device: AMD Radeon(TM) Graphics, vendor 1002, driver 2.0.353 (0x00800161), descriptor buffer
    shift: 0
    layout: size 48, binding 1 at 16, descriptor 16 bytes
    lengths=2,2,2,2 firsts=10,20,10,20
descriptor set:
    ...
    lengths=2,2,2,2 firsts=10,20,10,20
descriptor set, the first lane on the long view:
    ...
    lengths=8,8,8,8 firsts=20,10,20,10
REPRODUCED: OpArrayLength at a non-uniform index is wrong in: descriptor buffer; descriptor set; descriptor set, the first lane on the long view, while the loads reach the right elements
```

With `--vendor 8086` the Arc B580 (driver 0.406.958) prints the right lengths in every arm and exits 0.

## What we do about it

On vulkan, SGL asks such a length at uniform indices only, in its own SPIR-V and in the HLSL it hands DXC alike.
A fixed array of up to 16 elements asks each element and selects the lane's.
A larger or unbounded one takes a waterfall over the subgroup's first lane, in the compute and pixel stages.
[The debt issue](../../../libs/graphics/shaped-graphics-language/docs/issues/debt-sgl-vulkan-element-length-asked-at-uniform-indices.md) says what is still uncovered and when to remove it.

## Draft upstream report

Not posted; it is here for whoever files it with AMD.

> **Title:** OpArrayLength on a NonUniform-indexed storage buffer array element returns the first active lane's length (Vulkan)
>
> **System:** Radeon iGPU (RDNA 2), Ryzen 9 9950X3D2, driver 2.0.353, Windows 11.
>
> **Problem:** `views` is an array of storage buffers, and `i` and the access chain are decorated `NonUniform`.
> `OpArrayLength(OpAccessChain(views, i))` returns the same length on every lane: that of the element the first active lane indexes.
> Loads through the same access chain read each lane's own element.
> It happens with descriptor sets and with VK_EXT_descriptor_buffer alike.
>
> **Reproduction:** one C++ file and one SPIR-V assembly file, attached: 4 lanes over a buffer of 2 words and one of 8 return `2,2,2,2` instead of `2,8,2,8`.
>
> **Impact:** bounds checks over bindless buffers misjudge every lane but the first one's element.

## Re-check when

AMD ships a vulkan driver after 2.0.353: run `uv run run.py --vendor 1002`, and once it exits 0, remove the workaround the debt issue names.
