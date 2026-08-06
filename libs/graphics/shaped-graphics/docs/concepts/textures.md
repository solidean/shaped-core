# Concept: textures

A texture is a GPU-resident, immutable-shape resource, like [`raw_buffer`](../../src/shaped-graphics/resource/raw_buffer.hh) but carrying a texel grid instead of a byte range.
That grid is format, extents, mips, array slices and samples.
Two ideas shape the design: **raw resource vs typed wrapper**, and a **restrictive format set**.

## `raw_texture` vs `texture<Traits>`

The resource splits in two:

- [`raw_texture`](../../src/shaped-graphics/resource/raw_texture.hh) is the *stupid, general* base: it holds a `texture_description` and nothing else.
  A backend subclasses it and owns the native handle.
  Its API is minimal — getters plus the finalizer/expiry lifetime hooks it shares with `raw_buffer`.
  All shapes flow through the one type, and creation returns a `raw_texture_handle`.
- [`texture<Traits>`](../../src/shaped-graphics/resource/texture.hh) is a thin, *typed* value wrapper privately holding a `raw_texture_handle`.
  `Traits` is a single `texture_traits<Dim, Array, Cube, Multisampled>` *type*, carrying the shape as static members.
  It also carries a static `matches(desc)` that runs the runtime shape check against a `texture_description`, and the per-view parameter bags the factories take (see [views](views.md)).
  The shape-specific getters are gated with a trailing `requires`, mirroring typed-geometry's dimension-gating.
  So `depth()` exists only on a 3D texture and `array_layers()` only on an array, and misuse is a **compile error** rather than a runtime check.
  The typedefs `texture_2d`, `texture_cube_array`, … are the ergonomic names.

Why both: the raw type keeps the backend interface and the create path monomorphic — one virtual, one resource class.
The wrapper then gives call sites type safety without the backend ever knowing about `Traits`.
Wrap with `texture<Traits>::from_raw(handle)`, which asserts the raw shape matches, or its checked twin `try_from_raw`, which is nullopt on mismatch.
`raw()` hands back the `raw_texture_handle` for the general API; there is no implicit conversion.

### Shape is derived, not flagged

`texture_description` avoids redundant booleans.
`dimension` alone says which extents are meaningful: d1 → width, d2 → +height, d3 → +depth.
`array_layers` is a `cc::optional<int>` where `nullopt` means *not* an array, and a value (including `1`) means an array of that many slices.
So a plain 2D texture is distinct from a single-slice 2D array with no extra flag.
`sample_count > 1` means multisampled.
`is_cube` is the one genuinely-orthogonal flag — how the slices are interpreted, so a cube array is `is_cube=true` plus `array_layers=N`, meaning `6*N` faces internally.

## Restrictive `pixel_format`

[`pixel_format`](../../src/shaped-graphics/resource/pixel_format.hh) is deliberately small.
A format is included only when **every** realistic backend — DX12, Vulkan, Metal, WebGPU — has a direct equivalent.
That keeps the enum backend-neutral, with no value one API can represent and another cannot.

Consequences of that rule:

- **16-bit *norm*** formats are excluded — not WebGPU-core, since they need `texture-formats-tier1`.
  16-bit float and int are in.
- **`D24_UNORM_S8`** is excluded, being absent on Apple-Silicon Metal.
  `depth32_float_stencil8` is the portable depth-stencil format.
- **BC** (BC1–BC7) *is* included, mapping in all four APIs, but it is a **runtime capability** every backend gates.
  Vk `textureCompressionBC`, WGPU `texture-compression-bc`, Metal `supportsBCTextureCompression`.
  The enumerant always exists, and a future device-capability query will gate actual use.
  Mobile-only compression (ASTC / ETC / PVRTC) is out entirely.

When in doubt, leave a format out until a concrete need plus a capability query justify it.

## What exists today

Creation: `ctx.persistent.create_raw_texture(desc)` and `ctx.transient.create_raw_texture(desc)`
allocate a real GPU texture from a full `texture_description` — **dx12** via a committed
`ID3D12Resource`, **vulkan** via a dedicated `VkImage` (minimal, matching its buffer path). On top of
that, per-shape typed factories (`create_texture_2d`, `create_texture_cube`, … one per `texture<Traits>`
typedef, on both scopes, with `try_` twins — see [texture_descriptions.hh](../../src/shaped-graphics/resource/texture_descriptions.hh))
take a shape-specific description that exposes only the free parameters (cubes a single `.size`, cube
arrays a `.cube_count`, MS a `.sample_count`), expand it to a full `texture_description`, and return the
wrapped `texture<Traits>` directly.

Textures also carry views ([views](views.md)), per-command-list layout tracking ([barriers](barriers.md)), and host↔device copies.
Those copies are [inline upload](upload.inline.md) / [inline download](download.inline.md) and their async siblings.
Deliberately **not** here yet:

- **Placed / transient-bump textures** — the transient scope allocates *dedicated* for now; a
  texture-capable transient `memory_heap` is the missing piece.
- **Device→device texture copies** (texture→texture `CopyTextureRegion`) — only host↔device copies exist.
- **Mip generation / format conversion** — belongs in shaped-rendering (sr), on top of these copies.

## See also

- [memory](memory.md) — the lifetime and placement axes a texture's backing memory is chosen on.
- [views](views.md) — how a shader reads a texture.
- [barriers](barriers.md) — the layout tracking that makes an explicit transition unnecessary.
