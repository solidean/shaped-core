---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_resources.cc
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_binding.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/raster_bound_routine.cc
  - file: examples/graphics/web-viewer/web-shell.html
---

# The webgpu backend makes WebGPU objects every frame, and Firefox runs out of descriptors before it collects them

The web-viewer drawing the glTF Research Scenes' modern hall in Firefox loses its device after 30-90 s of rendering at ~350 fps.
The page prints `webgpu out of memory error: Out of Memory`, then `Out of memory`, and the next frame throws.
What is left on screen then misses half its materials.

The page's `?census=1` counts WebGPU objects by kind, and shows what every frame makes:

- ~4 bind groups and ~7 texture views, which have no `destroy()`: a browser frees them only when its garbage collector runs;
- one texture and two buffers, destroyed explicitly;
- one command encoder.

In one run the first collection came 30 s in, with 66k bind groups and 116k views alive; collections came every few seconds after that.
Run before it, the device was lost 52 s in, which reads as a run whose first collection came too late.
Each live bind group and view holds descriptors in Firefox's D3D12 heaps, and running out is reported as out of memory.

Measured to rule out the rest:

- Native dx12 in the same bound mode holds 529 MiB flat over 3000 frames, and a WebGPU run under node (Dawn) draws 3000 frames without an error: both free an object when it is released.
- A canvas snapshot 2 s before the loss shows every material, compressed: the encode is not involved, though turning it off seemed to help in a shorter session.

Where the objects come from:

- `webgpu_texture::create_view` makes a new `GPUTextureView` for every binding and every render target, every time.
- `ctx.transient.create_texture_2d` makes a new texture each frame, such as `raster_bound_routine`'s depth target.
- `ctx.transient.create_binding_group` makes a new bind group each time, such as `raster_bound_routine`'s group 0 and each textured draw's group 1.

The directions, one per source:

- cache a texture's views on the texture;
- reuse a transient texture of the same description once its epoch completes;
- reuse a transient bind group whose entries did not change.

## Fixed in the working tree, 2026-11-09

All three are in the webgpu backend (`webgpu_reuse.cc`), under one rule: reused while asked for, released after an epoch without a request.
A texture keeps its whole-texture view for its lifetime and any other view until it is two epochs unused; `release_cached_view(s)` on `sg::raw_texture` is the escape hatch.
Transient textures and buffers return to a claimable side at expiry, and transient binding groups are keyed by the serials of the WebGPU objects they bind.

The same Firefox page, 150 s and 49.5k frames: 77 bind groups, 10 textures and 149 buffers created in all, against several per frame before, and no device error.
What still comes per frame is the canvas's own view and the command encoders.
`webgpu-reuse-test.cc` pins each mechanism by counting what the backend creates.

## Several resources of one description

sv's lights page, six raster_bound views of one size, still lost its device to "Out of memory" after ~1660 frames: 78 bind groups a frame were made new, with textures and buffers all reused.
`claim_texture` and `claim_buffer` took the first match and removed it unordered, which swaps the last claimable entry into its place.
Six views asking for identical transients got them back in a different order every frame, so no group's key ever recurred.
The claims now remove in order, and the incoming side is sorted by the order the epoch's requests were handed their objects.
Expiry order alone would not do, since a handle dropped mid-frame offers its object early; a reuse test pins both cases.
A group binding a TLAS was never reused either, though the root lives on the sg group and the WebGPU group holds no entry for it.
raster_bound's traced passes bind one per view every frame; such groups are reused now, which a raytracing test pins.
After all three, the lights page ran 150 s with no device error, where it had lost its device at 24 s.
