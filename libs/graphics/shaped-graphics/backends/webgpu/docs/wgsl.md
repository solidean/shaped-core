# Writing WGSL for sg

The WebGPU backend consumes WGSL source, written by hand until sg has a shading language of its own.
shaped-shader-library reflects a WGSL file with a declaration parser, so the rules below are what that parser and this backend agree on.

## One entry point per file

A WGSL file declares exactly one `@compute`, `@vertex` or `@fragment` function.
A package entry names the file and the stage, and the parser refuses a file with a second entry point rather than guessing which one it meant.
A raster pipeline therefore takes its vertex and fragment stages from two files.

## Groups 0 to 2 are yours, group 3 is sg's

A pipeline layout hands a caller three groups on every backend.
Group 3 is where the backend emulates what WebGPU does not have, and it is laid out like this:

```wgsl
// Inline constants: always binding 0, always a uniform.
struct Constants {
    scale: u32,
    add: u32,
}
@group(3) @binding(0) var<uniform> constants: Constants;

// A pipeline layout's bound_sampler at register N is binding N + 1.
@group(3) @binding(1) var point: sampler;
```

- **Inline constants** are `cmd.compute.set_inline_constants` / `cmd.raster.set_inline_constants`.
  The pipeline layout's `inline_constants` binding gives the block size, which must be a multiple of 4 and match the struct.
  The parser reports this binding without a group index, the way SPIR-V reflection reports a push-constant block, so callers written against vulkan work unchanged.
- **Register-bound samplers** from `pipeline_layout_description::static_samplers` land at `index + 1`, since binding 0 is the constants.
- **Anything else** in group 3 is refused by the parser.

A **name-matched static sampler** — one named in `create_binding_group_layout`'s `static_samplers` — stays in the group that declares it.
The backend binds its sampler object into every group built from that layout, so a caller never supplies it.

## Texture dimensions

**Every sg 1D texture is a WebGPU 2D texture of height 1.**
A shader reading one declares `texture_2d` and addresses row 0:

```wgsl
@group(0) @binding(0) var ramp: texture_2d<f32>;
let v = textureLoad(ramp, vec2i(i, 0), 0);
```

A 1D array is a `texture_2d_array` the same way.
WebGPU's own `texture_1d` allows no mips, no arrays and no storage or render use, which is why sg never creates one.

## Bindings a layout needs before any resource exists

WebGPU fixes some facts at layout creation that dx12 and vulkan take from the bound view, so the parser reads them from the declaration:

- a **storage texture's** texel format and access mode, from `texture_storage_2d<rgba8unorm, write>`, into `storage_format` and `storage_access`.
  Core WebGPU allows `read_write` only for `r32float`, `r32uint` and `r32sint`; any other format is `write` or `read`;
- a **sampled texture's** sample type, from `texture_2d<f32>`, `texture_depth_2d`, `texture_2d<u32>` and so on.
  An `f32` texture is filterable, so a 32-bit float texture that is only loaded should be bound through an unfilterable layout;
  `texture_multisampled_2d<f32>` is the exception, reported unfilterable because WebGPU never filters a multisampled texture;
- a **sampler's** kind, from `sampler` or `sampler_comparison`.
  Every `sampler` is reported `filtering`, since a declaration parser cannot see which texture it samples.
  A sampler used with a depth or unfilterable-float texture must be bound through a layout whose binding sets `sampler_type = sg::sampler_binding_type::non_filtering`.

## Module-scope order is free

WGSL lets a module-scope declaration come after its use, and the parser follows it: a `const` may sit below the `@workgroup_size`, `@group` or `array<T, N>` that names it.
What it evaluates is a lone integer literal or the name of a `const` bound to one; anything larger is refused as an expression sg does not evaluate.

## What is refused

- `binding_array<...>`, since WebGPU core has no binding arrays — check `ctx.supports(sg::feature::binding_arrays)`;
- `texture_external`;
- a second entry point in one file;
- geometry and tessellation stages, which WebGPU does not have.

## Vertex inputs

A vertex attribute's `@location` is its index in the pipeline's `vertex_input_layout::attributes`, the same rule the vulkan backend follows for SPIR-V.
The HLSL semantic an attribute carries is ignored here.
