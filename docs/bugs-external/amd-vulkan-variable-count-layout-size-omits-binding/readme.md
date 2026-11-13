# AMD: a descriptor buffer layout's size leaves out its variable-count binding

**Status:** open, reproduced standalone, not yet filed upstream, worked around in shaped-graphics; measured on 2026-11-08.
**Affects:** AMD vulkan driver 2.0.353 (`0x00800161`), the Radeon iGPU of a Ryzen 9 9950X3D2 (RDNA 2), Windows 11, VK_EXT_descriptor_buffer.
**Found by:** `shaped-graphics-test` on that GPU, `SC_GPU=amd`: a staging group over an unbounded binding array wrote more bytes than its layout's size.

## What happens

`vkGetDescriptorSetLayoutSizeEXT` of a layout whose last binding has `VARIABLE_DESCRIPTOR_COUNT` returns that binding's offset, as though it held no descriptors.
The specification says the opposite:

> If any `binding` in `layout` is VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT, the returned size includes space for the maximum `descriptorCount` descriptors as declared for that `binding`.

A caller sizing its descriptor buffer by the returned size gets a range too small to hold the binding's descriptors.

## The trigger

Each row is one layout: a storage buffer at binding 0, and binding 1 holding `count` storage buffers, `PARTIALLY_BOUND` in every row.
The least size the spec allows is binding 1's offset plus `count` times `storageBufferDescriptorSize`.

| binding 1 | count | AMD size | least | Intel Arc B580 size |
|---|---|---|---|---|
| fixed | 4 | 80 | 80 | 320 |
| fixed | 1024 | 16,400 | 16,400 | 65,600 |
| fixed | 65,536 | 1,048,592 | 1,048,592 | 4,194,368 |
| variable | 4 | **16** | 80 | 320 |
| variable | 1024 | **16** | 16,400 | 65,600 |
| variable | 65,536 | **16** | 1,048,592 | 4,194,368 |

Intel's descriptor is 64 bytes and AMD's 16, so the two columns differ by design; each is compared against its own least.
The flag alone is the trigger: the same count without it is sized right, and the offset comes back right either way.

## Why it is not our bug

`repro.cc` creates the layouts and asks the two queries, nothing more, against the Vulkan loader alone.
The quoted sentence is the specification's own, and Intel's driver answers by it.

## Reproducing it

    uv run run.py [--vulkan-sdk <dir>] [--vendor <hex PCI id>]

It builds `repro.cc` with clang-cl against the SDK's headers and `vulkan-1.lib`, and runs it once.
`--vulkan-sdk` defaults to `$VULKAN_SDK`, then to the newest `~/tools/vulkan-sdk/<version>`.
`--vendor 1002` picks the AMD device; the first discrete GPU otherwise.
It exits 1 when a variable-count layout is smaller than the least, and 0 when none is.
On the machine above, with `--vendor 1002`, it prints:

```
device: AMD Radeon(TM) Graphics, vendor 1002, driver 2.0.353 (0x00800161), storage buffer descriptor 16 bytes
fixed    count     4: size       80, binding 1 at   16, least       80
fixed    count  1024: size    16400, binding 1 at   16, least    16400
fixed    count 65536: size  1048592, binding 1 at   16, least  1048592
variable count     4: size       16, binding 1 at   16, least       80  SHORT
variable count  1024: size       16, binding 1 at   16, least    16400  SHORT
variable count 65536: size       16, binding 1 at   16, least  1048592  SHORT
REPRODUCED: a variable-count layout is smaller than its binding's maximum count
```

## What we do about it

`vulkan_binding_group_layout::create` takes the larger of the driver's size and the variable binding's offset plus its capacity, which is the spec's own size on every driver.
It needs no removal: on a driver that follows the spec the two agree.
[vulkan_binding_group_layout.cc](../../../libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_binding_group_layout.cc) holds it.

## Draft upstream report

Not posted; it is here for whoever files it with AMD.

> **Title:** vkGetDescriptorSetLayoutSizeEXT excludes a VARIABLE_DESCRIPTOR_COUNT binding from the layout size
>
> **System:** Radeon iGPU (RDNA 2), Ryzen 9 9950X3D2, driver 2.0.353, Windows 11.
>
> **Problem:** for a layout of one storage buffer and then a variable-count array of N storage buffers, the returned size is 16, the array's offset, for every N.
> The specification says the size includes the binding's maximum `descriptorCount`; without the flag the same layout returns 16 + 16 × N.
>
> **Reproduction:** one C++ file, attached, which prints the size, the offset and the least size the spec allows for three counts, with and without the flag.

## Re-check when

AMD ships a vulkan driver after 2.0.353: run `uv run run.py --vendor 1002`.
Nothing of ours changes when it exits 0; the entry then moves to fixed.
