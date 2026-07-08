# Concept: views

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [memory](memory.md).

A **view** is a lightweight, strongly-typed value describing how a shader reads a (sub-range of a)
resource — "this buffer, these elements, as a read-only array of `T`". A rendering routine takes the
exact view it operates on (`readwrite_view<pixel>`), so a caller passes a slice of a resource without
a soup of overloads and without a struct whose fields could be mis-set. A view owns a
[`raw_buffer_handle`](../../src/shaped-graphics/fwd.hh) (it may outlive the call that made it) but no GPU
memory of its own — it is a pure value, produced by a `buffer.as_*()` factory.

## Typed by the element type

A buffer view is `access_view<T>`, where the **access class** is the type
(`uniform_view` / `readonly_view` / `readwrite_view`) and `T` is the element type of the array
(`readonly_view<particle>`) or the block type (`uniform_view<globals>`). There is no intermediate
"shape" wrapper: the raw / byte-addressed case is simply `T = byte` — the degenerate element (stride
1). `T` must satisfy the `view_element` concept: `byte`, or `sizeof(T) % 4 == 0`, because GPUs load at
4-byte (DWORD) alignment. Ranges passed to the factories are in **elements of `T`**.

`T` is a host-side safety + stride handle, not a wire format. Shader reflection reports a binding's
*layout and kind* (stride, size, structured/cbuffer/byteaddress) — never a C++ type — so at bind time
(a future milestone) `T` is *validated* against reflection (`sizeof(T)` == reflected stride, kind
matches), not replaced by it. Because the call site already fixed `T`, much of that check is
compile-time.

**Uniform blocks are stricter.** Constant buffers / UBOs have placement rules storage buffers don't,
so `uniform_view<T>` uses a tighter `uniform_element` concept and asserts the portable limits (the
strictest across backends, so a satisfying view binds everywhere): a block's size must be a **multiple
of 16** (std140 / HLSL cbuffer packing) and **at most 64 KiB** (D3D12 max CBV; WebGPU default max
binding) — both compile-time from `sizeof(T)`, which also rejects `uniform_view<byte>` — and its byte
**offset must be 256-byte aligned** (D3D12 CBV placement; WebGPU default; Vulkan's is ≤256 so 256 is
always valid) — a runtime assert, since the offset is a value.

## Two axes, drawn from what every shading language shares

The vocabulary is grounded in concepts common to HLSL / GLSL / Slang / MSL / WGSL — our baseline
shading language is undecided, so no one API's names leak into the surface. A buffer binding varies on
two axes the view captures:

- **Access class** (`view_class`): `uniform` (a small read-only block — cbuffer / UBO), `readonly`
  (read storage — SRV / read SSBO), `readwrite` (read-write storage — UAV / read-write SSBO). Mirrors
  [`buffer_usage`](../../src/shaped-graphics/types.hh)'s `uniform_buffer` / `readonly_buffer` /
  `readwrite_buffer`.
- **Layout** (`view_shape`, derived from `T`): `uniform_block`, `structured` (array strided by
  `sizeof(T)`), or `raw` (byte-addressed, `T = byte`).

Per-language mapping:

| view | HLSL / Slang | GLSL / Vulkan | MSL | WGSL |
|---|---|---|---|---|
| `uniform_view<T>` | `ConstantBuffer<T>` | std140 UBO | `constant T&` | `var<uniform>` |
| `readonly_view<T>` | `StructuredBuffer<T>` | `readonly buffer{ T[] }` | `const device T*` | `var<storage, read>` |
| `readwrite_view<T>` | `RWStructuredBuffer<T>` | `buffer{ T[] }` | `device T*` | `var<storage, read_write>` |
| `readonly_view<byte>` | `ByteAddressBuffer` | `readonly buffer{ uint[] }` | `const device uchar*` | `array<u32>` |
| `readwrite_view<byte>` | `RWByteAddressBuffer` | `buffer{ uint[] }` | `device uchar*` | (raw storage) |

Two things this factoring settles:

- **`readonly` vs `readwrite` is a superset, not a D3D quirk.** It is a shader qualifier everywhere
  (GLSL `readonly`/`writeonly`, WGSL `read`/`read_write`, MSL `const device`/`device`); only D3D also
  makes it a *separate descriptor* (SRV vs UAV). Vulkan/GLSL collapse both to one `STORAGE_BUFFER` and
  carry the difference in the qualifier + barrier state — but every backend needs the distinction for
  hazard tracking, so the view always carries it and each backend narrows as it likes.
- **structured vs raw is a *view* decision, not a hardware one.** Both are storage buffers everywhere
  except HLSL; the only difference is whether the view imposes an element type (`T`) or reads bytes
  (`T = byte`). `ByteAddressBuffer`-style (RAW, no stride) and `StructuredBuffer<T>` (stride
  `sizeof(T)`) *are* different backend descriptors, so the distinction survives — derived from `T`,
  not spelled in the type name.

The relationship a caller must uphold: a buffer's creation `buffer_usage` must be a **superset** of
every view's access (a `readwrite_view` requires `readwrite_buffer`, etc.). The factories assert this.

## The erased `raw_view`

Every typed view converts (`to_raw()`, or implicitly) into one plain
[`raw_view`](../../src/shaped-graphics/views.hh) — a tagged struct (access + shape enums + handle +
params) that a backend `switch`es on to build its native descriptor. The type safety lives entirely
in the typed views; users never touch `raw_view`, only the backend does.

## Texture views

Textures have views too, but they don't fit the buffer `<T>` slot (there is no element type — a texel
`pixel_format` and a subresource range instead). So they are distinct, non-templated types —
`texture_readonly_view` (sampled / SRV) and `texture_readwrite_view` (storage / UAV) — each carrying
`{raw_texture_handle, texture_view_dimension, pixel_format, subresource_range}` (plus a
`depth_slice_range` on the storage view for 3D) and erasing to a `raw_view` with `shape == texture`.

### The view *dimension* is a reinterpretation, not the texture's shape

A view carries an explicit `texture_view_dimension` — the shader-facing declaration (HLSL
`Texture2D` / `Texture2DArray` / `TextureCube` / `Texture2DMS…`; Vulkan `VkImageViewType`; D3D
`SRV/UAV_DIMENSION`) — because several selections *change what the shader sees*, not just which
subresources are visible. Binding one slice of a 2D array as `Texture2D` is a **different binding** than a
one-layer `Texture2DArray` window; likewise a single cube face → `Texture2D`, or one cube of a cube array
→ `TextureCube`. A subresource range alone can't tell those apart, so the backend switches on
`view_dimension` rather than re-deriving from the texture's `description`. (D3D12 caveat: the non-array
dimensions have no base-slice field, so a *non-zero* first slice promotes to the size-1 array form — same
texels, still declared as the requested dimension in the shader.)

### The factory surface

The factories live on the typed `texture<Traits>` wrapper, `requires`-gated by shape so misuse is a
compile error (mirroring the wrapper's `height()` / `depth()`):

- **Sampled (SRV):** `as_readonly_view(first_mip, mip_count)` on any shape (whole, natural dimension);
  `as_readonly_view(array_range)` and `as_readonly_slice_view(slice)` on non-cube arrays;
  `as_readonly_face_view(face)` on cubes; `as_readonly_cube_view(cube)`,
  `as_readonly_cube_range_view(cube_range)` and `as_readonly_face_view(cube, face)` on cube arrays. A mip
  *range* is selectable (default: all mips). Multisampled textures **are** sampleable (`Texture2DMS…`) —
  they just carry no mip range, and a multisampled cube samples as a `Texture2DMSArray` (there is no
  `TextureCubeMS`).
- **Storage (UAV):** only where `!Traits.is_multisampled` (D3D12 forbids MSAA UAVs), always a **single mip
  level**, and a cube is written as a 2D array (no cube UAV). `as_readwrite_view(mip)` /
  `as_readwrite_view(mip, array_range)` / `as_readwrite_slice_view` / `as_readwrite_face_view`, plus
  `as_readwrite_depth_slice_view(depth_slice_range)` on 3D textures (the depth / W/Z axis — D3D12's
  `FirstWSlice`/`WSize` window — a UAV-only axis with no SRV analog, tracked separately from the
  subresource range since a whole 3D mip is one subresource for hazard purposes).

`binding_type::sampled_texture` / `storage_texture` map to `(readonly, texture)` / `(readwrite, texture)`.
When a texture is bound to a compute dispatch, the barrier system transitions it to the layout its access
implies (sampled → `shader_read`, storage → `storage`) via `shader_layout_of`.

Deferred: **aspect (depth/stencil) selection + format reinterpretation** on sampled views (depth-as-SRV
needs a typeless resource), **render_target / depth_stencil** views (a graphics pipeline / render pass
consumes them), **samplers** (a separate descriptor heap), and **texel buffers** (`Buffer<T>` /
`samplerBuffer` — a format-decoded linear buffer).

## See also

- [views.hh](../../src/shaped-graphics/views.hh) — the view types, `view_class` / `view_shape`, and `raw_view`.
- [buffer.hh](../../src/shaped-graphics/raw_buffer.hh) — the `buffer.as_*()` view factories.
- [memory](memory.md) — the resource-backing model views sit on top of.
