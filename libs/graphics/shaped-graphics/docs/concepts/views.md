# Concept: views

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [memory](memory.md).

A **view** is a lightweight, strongly-typed value describing how a shader reads a (sub-range of a)
resource — "this buffer, these elements, as a read-only array of `T`". A rendering routine takes the
exact view it operates on (`readwrite_buffer_view<pixel>`), so a caller passes a slice of a resource without
a soup of overloads and without a struct whose fields could be mis-set. A view owns a
[`raw_buffer_handle`](../../src/shaped-graphics/fwd.hh) (it may outlive the call that made it) but no GPU
memory of its own — it is a pure value, produced by a `buffer.as_*()` factory.

## Typed by the element type

A buffer view is `<access>_buffer_view<T>`, where the **access class** is the type
(`uniform_buffer_view` / `readonly_buffer_view` / `readwrite_buffer_view`) and `T` is the element type of
the array (`readonly_buffer_view<particle>`) or the block type (`uniform_buffer_view<globals>`). There is no intermediate
"shape" wrapper: the raw / byte-addressed case is simply `T = byte` — the degenerate element (stride
1). `T` must satisfy the `view_element` concept: `byte`, or `sizeof(T) % 4 == 0`, because GPUs load at
4-byte (DWORD) alignment. Ranges passed to the factories are in **elements of `T`**.

`T` is a host-side safety + stride handle, not a wire format. Shader reflection reports a binding's
*layout and kind* (stride, size, structured/cbuffer/byteaddress) — never a C++ type — so at bind time
(a future milestone) `T` is *validated* against reflection (`sizeof(T)` == reflected stride, kind
matches), not replaced by it. Because the call site already fixed `T`, much of that check is
compile-time.

**Uniform blocks are stricter.** Constant buffers / UBOs have placement rules storage buffers don't,
so `uniform_buffer_view<T>` uses a tighter `uniform_element` concept and asserts the portable limits (the
strictest across backends, so a satisfying view binds everywhere): a block's size must be a **multiple
of 16** (std140 / HLSL cbuffer packing) and **at most 64 KiB** (D3D12 max CBV; WebGPU default max
binding) — both compile-time from `sizeof(T)`, which also rejects `uniform_buffer_view<byte>` — and its byte
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
| `uniform_buffer_view<T>` | `ConstantBuffer<T>` | std140 UBO | `constant T&` | `var<uniform>` |
| `readonly_buffer_view<T>` | `StructuredBuffer<T>` | `readonly buffer{ T[] }` | `const device T*` | `var<storage, read>` |
| `readwrite_buffer_view<T>` | `RWStructuredBuffer<T>` | `buffer{ T[] }` | `device T*` | `var<storage, read_write>` |
| `readonly_buffer_view<byte>` | `ByteAddressBuffer` | `readonly buffer{ uint[] }` | `const device uchar*` | `array<u32>` |
| `readwrite_buffer_view<byte>` | `RWByteAddressBuffer` | `buffer{ uint[] }` | `device uchar*` | (raw storage) |

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
every view's access (a `readwrite_buffer_view` requires `readwrite_buffer`, etc.). The factories assert this.

## The erased `raw_view`

Every typed view converts (`to_raw()`, or implicitly) into one
[`raw_view`](../../src/shaped-graphics/views.hh) — a `std::variant` over one cohesive payload per
resource kind: `raw_buffer_view` (access + shape + buffer + byte layout), `raw_texture_view` (access +
texture + dimension + format + range), and `raw_tlas_view` (the TLAS). A backend `std::visit`s / `get_if`s
the active arm to build its native descriptor; `access_of(rv)` / `shape_of(rv)` read the active arm's
access / layout (what `accepts()` checks). The type safety lives in the typed views; the raw arms are
also the directly-usable "raw" binding vocabulary for tooling that builds bindings without the wrappers.
(`std::variant` for now — likely a `cc::variant` once that lands.)

Between the fully-typed leaves and the erased `raw_view` sits an optional **access-erased middle**:
`buffer_view<T>` and `texture_view<Traits>` keep the resource typing (element / view dimension) but carry
the access class as a runtime field. Each leaf converts to it implicitly, and it erases on to `raw_view` —
useful for code that takes "any access" of a given buffer / texture view.

### Recovering a typed view from the erased form

Erasure is not one-way: each layer offers `as_<access>()` / `try_as_<access>()` back up toward the leaves.
`as_*` asserts, `try_*` returns a `cc::optional` (nullopt on mismatch) — the checked twin for genuinely
runtime input.

- **Access-erased middle → leaf.** `buffer_view<T>::as_readonly()` / `as_readwrite()` / `as_uniform()` and
  `texture_view<Traits>::as_readonly()` / `as_readwrite()` pin the runtime access class to the matching
  compile-time leaf. The resource typing is already fixed, so only the access is being committed.
- **Erased arm → leaf.** `raw_buffer_view::as_readonly<T>()` (you supply the element `T`) and
  `raw_texture_view::as_readonly<Traits>()` (you supply `Traits`; it also checks the runtime `view_dimension`
  matches `Traits::dimension`). These delegate to the middle for the access check and field mapping.
- **`raw_view` → leaf in one call.** The free functions `as_readonly_buffer<T>(rv)` /
  `as_readwrite_buffer<T>` / `as_uniform_buffer<T>` and `as_readonly_texture<Traits>` /
  `as_readwrite_texture<Traits>` (each with a `try_` twin) `get_if` the matching arm, then re-type it. The
  `try_` twin fails (nullopt) both when the variant holds a *different* resource arm and when the access
  class does not match — the two ways a genuinely-erased binding can be the wrong thing.

The re-type is a reinterpret: `T` / `Traits` are caller-asserted (there is no element/dimension tag stored to
cross-check against), so this is the deliberate, checked counterpart of the free erasure the other direction.
The one thing it *can* cross-check is the byte layout: a buffer recovery asserts `T` matches the view's shape —
`byte` ⇔ a raw (byte-addressed) view, any other `T` ⇔ a structured view whose stride is exactly `sizeof(T)` —
so picking the wrong element size is a loud error, not a silently wrong element count.

For the raw *resource* (not a view), the same inverse exists at the wrapper level: `raw_buffer::as_buffer<T>()`
and `raw_texture::as_texture_2d()` (one per shape typedef), each with a `try_` twin.

### Placement rules: a view is a subrange, so it carries the binding rules

`buffer<T>` is a *whole buffer* recast like a `span` — it has essentially no placement constraints (which is
why a `buffer<u16>` index buffer is perfectly legal). A **view**, by contrast, is a *subrange*, so it inherits
the portable binding rules. Every shader-facing storage view (readonly / readwrite, raw or structured) asserts:

- **`offset % 256 == 0`** — WebGPU's `minStorageBufferOffsetAlignment` is 256, and Vulkan permits an
  implementation to require up to 256 (its required-limit *maximum*, so real hardware does), making 256 the
  only portable start. See `storage_buffer_offset_alignment`.
- **`size % 4 == 0`** — a WebGPU storage binding's size must be a multiple of 4.

A **structured** view (any non-`byte` element type) adds a second, orthogonal rule: it addresses by *element
index*, not byte offset — D3D12 literally computes `FirstElement = offset / stride`. So its offset must **also**
be a multiple of the stride, and its size a whole number of elements. A structured view therefore cannot start
partway into an element. (The two rules together mean `offset % lcm(256, stride) == 0`.)

The 256 rule is deliberately the *portable floor*, hardcoded rather than queried per device — the same approach
as `uniform_buffer_offset_alignment`. It fails loudly on your dx12 dev box rather than surfacing later on
WebGPU. Exempt: `buffer<T>` itself, and the draw-input views (`as_vertex_buffer` / `as_index_buffer`), which are
not storage bindings and have their own rules. **Escape hatch:** build the erased `raw_buffer_view` aggregate
yourself if you knowingly target only backends with looser rules.

Because a subrange can legitimately fail these rules on an offset a caller computed at runtime (a suballocator,
say), every storage / uniform factory has a **`try_` twin** returning `cc::optional`: `try_as_raw_readonly`,
`try_as_raw_readwrite`, `try_as_raw_uniform_buffer` (each overload, including the whole-buffer form) and
`buffer<T>::try_as_readonly_buffer` / `try_as_readwrite_buffer` / `try_as_uniform_buffer`. The split follows the
rest of the API (`try_from_raw`, `try_as_readonly`): a bad **range** — bounds, alignment, size, stride — is a
runtime condition and yields nullopt, while a missing `buffer_usage` flag stays a hard assert, since the usage
was chosen at creation and getting it wrong is a contract bug. Note even the whole-buffer overloads can fail:
`as_raw_readonly()` on a 70-byte buffer trips `size % 4`. Draw-input views have no twin — they can only fail
bounds.

### Use raw + in-shader `Load<T>` for heterogeneous buffers

Because a structured view can't start partway into an element, it's the wrong tool for a **heterogeneous
buffer** — one packing different objects at hand-chosen byte offsets (a header, then an array, then something
else). For that, use a **raw** (byte-addressed) view: `as_raw_readonly({.offset, .size})` with no stride yields
a `readonly_buffer_view<byte>` — a `ByteAddressBuffer` — which you type per access in the shader with
`buf.Load<T>(byteOffset)`. The per-access `Load` offset only needs 4-byte alignment, so each object sits at its
own byte offset with no `sizeof(T)` constraint: `byte` at the view, `T` at the load.

Since the *view's* start still obeys the 256-byte rule, the usual shape is **one raw view over the whole
buffer** (start 0, trivially aligned) with *all* per-object addressing done by `Load` — the view-start
alignment then never bites. Offsetting the raw view itself is only for coarse 256-aligned sub-regions.

Portability note: `ByteAddressBuffer.Load<T>` is a first-class HLSL feature (and Vulkan/Metal have equivalents
— `buffer_reference`, pointer casts), but **WGSL has no byte-address buffer and no templated load**. On WebGPU
you must fall back to a typed storage buffer (i.e. the stride-aligned structured layout) and unpack by hand.

## Texture views

Textures have views too, but instead of an element type `T` they are typed by `Traits` — a
`texture_view_traits<Dim>` naming the shader-facing dimension (`tv_2d` / `tv_cube` / `tv_2d_array` / …),
which is the only compile-time part (the texel `pixel_format` and subresource range stay runtime). So the
leaves are `readonly_texture_view<Traits>` (sampled / SRV) and `readwrite_texture_view<Traits>` (storage /
UAV, `Traits::dimension` constrained to a `storage_view_dimension` — no cube, no MSAA), each carrying
`{raw_texture_handle, pixel_format, subresource_range}` (plus a `depth_slice_range` on the storage view for
3D) and erasing to a `raw_view` holding a `raw_texture_view`. `texture<Traits>::as_*_view()` computes the
view dimension at compile time and returns the precisely-typed leaf.

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
compile error (mirroring the wrapper's `height()` / `depth()`). Rather than a positional overload set,
each factory takes **one shape-specific parameter bag** — `Traits::read_only_params`,
`Traits::read_write_2d_params`, … — an aggregate that names only the axes that shape has. A plain 2D
texture's `read_only_params` is `{ view_range mips; }`; a 2D array's adds `slices`; a cube array's adds
`cubes`. Selecting a nonsensical axis (e.g. `.slices` on a non-array) is a compile error, not a
silently-ignored field. A `view_range` is `{ int start = 0; int count = -1 }` where `count < 0` means "to
the end", so the whole-axis default is `{}`.

- **Natural views** — the texture's own dimension: `as_readonly_view(params = {})` on any shape,
  `as_readwrite_view(params = {})` where `!Traits::is_multisampled`. The params carry the in-dimension
  sub-selection: a mip range (sampled) or single mip (storage), an array-slice range, a cube range, or a
  3D depth-slice window (`depth_slices`, D3D12's `FirstWSlice`/`WSize` — tracked outside the subresource
  range since a whole 3D mip is one subresource for hazard purposes).
- **Reinterpreting views** — bind one slice / face / cube as a lower dimension, each with its own params
  bag: `as_readonly_2d_view` / `as_readwrite_2d_view` (one slice/face → `Texture2D`), `as_readonly_1d_view`
  / `as_readwrite_1d_view` (one slice of a 1D array → `Texture1D`), `as_readonly_cube_view` (one cube of a
  cube array → `TextureCube`), and `as_readonly_2d_array_view` (a cube / cube array's faces as a flat
  `Texture2DArray` — the sampled counterpart to how a cube's storage view already binds). These are what
  make "slice 3 as `Texture2D`" distinct from a size-1 array window.

Multisampled textures **are** sampleable (`Texture2DMS…`) — their params just have no separate mip axis
(one mip level), and a multisampled cube samples as a `Texture2DMSArray` (there is no `TextureCubeMS`).
Storage views never apply to MSAA (D3D12 forbids MSAA UAVs) and a cube is written as a 2D array (no cube
UAV).

`binding_type::readonly_texture` / `readwrite_texture` map to `(readonly, texture)` / `(readwrite, texture)`.
When a texture is bound to a compute dispatch, the barrier system transitions it to the layout its access
implies (sampled → `shader_read`, storage → `storage`) via `shader_layout_of`.

### Render-target / depth-stencil views

Binding a texture as a graphics-pipeline **color** or **depth/stencil** target is a *different kind* of
view: it is never shader-visible, never enters a binding group / descriptor table, and is bound through the
output-merger (`OMSetRenderTargets` / dynamic-rendering attachments). So it does **not** erase to `raw_view`
— `render_target_view` / `depth_stencil_view` (in [views.hh](../../src/shaped-graphics/views.hh)), plain
value types (holding the `raw_texture_handle`, so they keep the texture alive) with getters `texture()` /
`dimension()` / `format()` / `range()` / `width()` / `height()`. A backend consumes the typed view directly.

The factories mirror the storage-view shape — a single mip level + an array-slice range, 2D-shaped only
(`as_render_target_view` / `as_depth_stencil_view` on any 2D texture incl. cubes and MSAA;
`as_render_target_2d_view` / `as_depth_stencil_2d_view` bind one layer / cube face as a `Texture2D`). Unlike
storage views, MSAA **is** allowed (`Texture2DMS…`). The factory asserts the texture's usage
(`render_target` / `depth_stencil`) and its format: a render target must be a renderable color format
(`is_render_target_format` — non-depth, non-compressed), a depth-stencil target a depth format
(`is_depth_format`). The render-pass / OMSetRenderTargets *consumer* is future work; today the DX12 backend
lands the descriptors into non-shader-visible RTV/DSV heaps (`dx12_context::create_dx12_render_target_view`
/ `create_dx12_depth_stencil_view`).

Deferred: **aspect (depth/stencil) selection + format reinterpretation** on sampled views (depth-as-SRV
needs a typeless resource), the **render-pass consumer** for the render-target / depth-stencil views above, and **texel
buffers** (`Buffer<T>` / `samplerBuffer` — a format-decoded linear buffer). **Samplers** are supported (see
[bindings](bindings.md) and `sampler.hh`) — they are not views, so they live outside this concept: a
`sampler` bound static (baked into a layout's root signature) or dynamic (per binding_group, in a separate
sampler descriptor heap).

## See also

- [views.hh](../../src/shaped-graphics/views.hh) — the view types (shader-facing views plus `render_target_view` / `depth_stencil_view`), `view_class` / `view_shape`, and `raw_view`.
- [buffer.hh](../../src/shaped-graphics/buffer.hh) — the typed `buffer<T>.as_*()` view factories (raw_buffer itself has only the byte-level `as_raw_*`).
- [memory](memory.md) — the resource-backing model views sit on top of.
