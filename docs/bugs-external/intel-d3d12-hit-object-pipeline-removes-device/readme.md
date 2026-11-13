# Intel D3D12: creating a pipeline that reads a hit object before invoking it removes the device

**Status:** open, not yet filed upstream and not yet reduced; the one test that builds such a pipeline skips on Intel's dx12.
**Affects:** Intel Arc B580, driver 32.0.101.9150, Windows 11, through the D3D12 Agility SDK 1.620.1 at shader model 6.9; vulkan on the same GPU is not affected.
**Found by:** `sg - an SGL hit object says what its closest hit is then handed`, once the Agility SDK made dx12 report shader execution reordering.

## What happens

`create_raytracing_pipeline` spends about 21 s in the driver's compiler, `igc-default64.dll` under `OpenCompilerDX_OCL`, and the device is then removed.
The reason is `DXGI_ERROR_DRIVER_INTERNAL_ERROR` (0x887A0020), and the debug layer has nothing to say but the removal.
It happens on every run, five of five on 2026-11-07, and Windows logs no driver timeout (4101) for it.
Every later call on the device fails, so the whole binary's dx12 contexts are lost with it.

## Why it is not our bug

It happens on both dx12 routes.
The `dx12-hw` context builds the pipeline from DXC's DXIL of SGL's HLSL, and `dx12-native-dxil` from SGL's own DXIL.
Both pass DXC's validator, and the same SGL source draws correctly on vulkan, where reordering is native, through both of its routes.
A removal at pipeline creation, with a validated module and the driver's own internal-error code, is the driver's.

## The shape

`inspected_primary` in `libs/graphics/shaped-graphics/tests/shaders/sgl/raytracing_pipeline.sgl` traces into a hit object.
It reads `is_hit`, `instance_id`, `primitive_index` and `t` from it, reorders by one of them, invokes it, and reads `is_miss` afterwards.

Two neighbours on the same pipeline layout create and draw correctly on this driver:

- `hit_reordered_primary` traces into a hit object, reorders by it and a hint, and invokes it, reading nothing from it.
- `reordered_primary` reorders by a hint before an ordinary trace.

So the reads are the likely trigger, but no reduced pipeline has shown which one.

## The workaround

None in sg: the test skips where the backend is dx12, the adapter is Intel hardware and the context reports shader execution reordering.
An application that reads a hit object before invoking it on this driver should not do so on dx12.
