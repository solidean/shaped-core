# Concept: textures

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [memory](memory.md), [views](views.md), [barriers](barriers.md).

A texture is a GPU-resident, immutable-shape resource — like [`raw_buffer`](../../src/shaped-graphics/raw_buffer.hh),
but with a texel grid (format, extents, mips, array slices, samples) instead of a byte range. Textures
are shaped by two ideas that are worth explaining: **raw resource vs typed wrapper**, and a
**restrictive format set**.

## `raw_texture` vs `texture<Traits>`

The resource splits in two:

- [`raw_texture`](../../src/shaped-graphics/raw_texture.hh) is the *stupid, general* base — it holds a
  `texture_description` and nothing else; a backend subclasses it and owns the native handle. Its API is
  minimal (getters + the finalizer/expiry lifetime hooks it shares with `raw_buffer`). All shapes flow
  through the one type; creation returns a `raw_texture_handle`.
- [`texture<Traits>`](../../src/shaped-graphics/texture.hh) is a thin, *typed* value wrapper that
  privately holds a `raw_texture_handle`. `Traits` is a single `texture_traits<Dim, Array, Cube,
  Multisampled>` *type* — it carries the shape as static members, a static `matches(desc)` that runs the
  runtime shape check against a `texture_description`, and the per-view parameter bags the factories take
  (`read_only_params`, `read_write_2d_params`, …; see [views](views.md)). The shape-specific getters are
  gated with a trailing `requires`, mirroring typed-geometry's dimension-gating — so `depth()` exists only
  on a 3D texture, `array_layers()` only on an array, and misuse is a **compile error**, not a runtime
  check. The typedefs (`texture_2d`, `texture_cube_array`, …) are the ergonomic names.

Why both: the raw type keeps the backend interface and the create path monomorphic (one virtual, one
resource class), while the wrapper gives call sites type safety without the backend ever knowing about
`Traits`. Wrap with `texture<Traits>::from_raw(handle)` (asserts the raw shape matches) or its checked
twin `try_from_raw` (nullopt on mismatch); `raw()` hands back the `raw_texture_handle` for the general
API (there is no implicit conversion).

### Shape is derived, not flagged

`texture_description` avoids redundant booleans. `dimension` alone says which extents are meaningful
(d1 → width; d2 → +height; d3 → +depth). `array_layers` is a `cc::optional<int>`: `nullopt` means *not*
an array, a value (including `1`) means an array of that many slices — so a plain 2D texture is distinct
from a single-slice 2D array with no extra flag. `sample_count > 1` means multisampled. `is_cube` is the
one genuinely-orthogonal flag (how the slices are interpreted; a cube array is `is_cube=true` +
`array_layers=N`, i.e. `6*N` faces internally).

## Restrictive `pixel_format`

[`pixel_format`](../../src/shaped-graphics/pixel_format.hh) is deliberately small: a format is included
only when **every** realistic backend (DX12 / Vulkan / Metal / WebGPU) has a direct equivalent. This
keeps the enum backend-neutral — no value that one API can represent and another can't.

Consequences of that rule:

- **16-bit *norm*** formats are excluded (not WebGPU-core — they need `texture-formats-tier1`). 16-bit
  float and int are in.
- **`D24_UNORM_S8`** is excluded (absent on Apple-Silicon Metal); `depth32_float_stencil8` is the
  portable depth-stencil format.
- **BC** (BC1–BC7) *is* included — it maps in all four APIs — but it is a **runtime capability** every
  backend gates (Vk `textureCompressionBC`, WGPU `texture-compression-bc`, Metal
  `supportsBCTextureCompression`). The enumerant always exists; a future device-capability query will
  gate actual use. Mobile-only compression (ASTC/ETC/PVRTC) is out entirely.

The full excluded list and rationale lives in the implementation plan; the short version is *when in
doubt, leave it out until a concrete need plus a capability query justify it.*

## What exists today

Creation: `ctx.persistent.create_raw_texture(desc)` and `ctx.transient.create_raw_texture(desc)`
allocate a real GPU texture from a full `texture_description` — **dx12** via a committed
`ID3D12Resource`, **vulkan** via a dedicated `VkImage` (minimal, matching its buffer path). On top of
that, per-shape typed factories (`create_texture_2d`, `create_texture_cube`, … one per `texture<Traits>`
typedef, on both scopes, with `try_` twins — see [texture_descriptions.hh](../../src/shaped-graphics/texture_descriptions.hh))
take a shape-specific description that exposes only the free parameters (cubes a single `.size`, cube
arrays a `.cube_count`, MS a `.sample_count`), expand it to a full `texture_description`, and return the
wrapped `texture<Traits>` directly.

Since first landing, textures have grown views ([views.md](views.md)), per-command-list layout tracking
([barriers.md](barriers.md)), and host↔device copies ([upload.inline.md](upload.inline.md) /
[download.inline.md](download.inline.md) and their async siblings). Still deliberately **not** here yet:

- **Placed / transient-bump textures** — the transient scope allocates *dedicated* for now; a
  texture-capable transient `memory_heap` is the missing piece.
- **Device→device texture copies** (texture→texture `CopyTextureRegion`) — only host↔device copies exist.
- **Mip generation / format conversion** — belongs in shaped-rendering (sr), on top of these copies.
