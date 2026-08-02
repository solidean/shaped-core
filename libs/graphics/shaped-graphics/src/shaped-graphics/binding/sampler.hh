#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>

/// Backend-neutral texture **sampler** description: how a shader reads a texture (filtering, addressing,
/// LOD, optional depth comparison). A sampler is a small immutable state value, not a GPU resource that
/// owns memory. It reaches a shader two ways (see libs/graphics/shaped-graphics/docs/concepts/bindings.md):
///   - **static** — declared on a binding_group_layout and baked into the pipeline layout's root signature;
///   - **dynamic** — supplied per binding_group (a `named_sampler`), written into a sampler descriptor heap.

namespace sg
{
/// Per-axis filtering mode (minification, magnification, and between mip levels).
enum class sampler_filter
{
    nearest, ///< point sampling (GL_NEAREST / D3D POINT / Vk NEAREST)
    linear,  ///< linear interpolation (GL_LINEAR / D3D LINEAR / Vk LINEAR)
};

/// How texture coordinates outside [0, 1) are resolved, per axis.
enum class sampler_address_mode
{
    repeat,            ///< wrap (Vk REPEAT / D3D WRAP)
    mirror_repeat,     ///< wrap, mirroring every other tile (Vk MIRRORED_REPEAT / D3D MIRROR)
    clamp_edge,        ///< clamp to the edge texel (Vk CLAMP_TO_EDGE / D3D CLAMP)
    clamp_border,      ///< return `border_color` outside (Vk CLAMP_TO_BORDER / D3D BORDER)
    mirror_clamp_edge, ///< mirror once, then clamp (Vk MIRROR_CLAMP_TO_EDGE / D3D MIRROR_ONCE)
};

/// The fixed border color used by `clamp_border` addressing. The portable set (the three every backend
/// supports as a static sampler); an arbitrary float4 border is deferred.
enum class sampler_border_color
{
    transparent_black, ///< (0, 0, 0, 0)
    opaque_black,      ///< (0, 0, 0, 1)
    opaque_white,      ///< (1, 1, 1, 1)
};

/// Comparison function for a comparison ("shadow") sampler — compares the fetched texel against the
/// reference value the shader passes to `SampleCmp`. Also the vocabulary a future depth test will reuse.
enum class compare_op
{
    never,
    less,
    equal,
    less_equal,
    greater,
    not_equal,
    greater_equal,
    always,
};

/// An immutable sampler state. Defaults are a trilinear repeating sampler with no anisotropy and no depth
/// comparison — the common case. Value type: cheap to copy and compare.
struct sampler
{
    /// Sentinel `max_lod` meaning "no upper mip clamp" (FLT_MAX).
    static constexpr float lod_max = 3.4028235e38f;

    sampler_filter min_filter = sampler_filter::linear; ///< minification filter
    sampler_filter mag_filter = sampler_filter::linear; ///< magnification filter
    sampler_filter mip_filter = sampler_filter::linear; ///< filtering between mip levels

    sampler_address_mode address_u = sampler_address_mode::repeat;
    sampler_address_mode address_v = sampler_address_mode::repeat;
    sampler_address_mode address_w = sampler_address_mode::repeat;

    float mip_lod_bias = 0.0f; ///< added to the computed mip level
    u32 max_anisotropy = 1;    ///< 1 = anisotropy off; > 1 enables anisotropic filtering (capped per backend)
    float min_lod = 0.0f;      ///< lower mip-level clamp
    float max_lod = lod_max;   ///< upper mip-level clamp (lod_max = unclamped)

    /// Set for a comparison ("shadow") sampler — the filters then apply to the comparison result.
    cc::optional<compare_op> compare = {};

    /// Border texel returned by `clamp_border` addressing; ignored by the other address modes.
    sampler_border_color border_color = sampler_border_color::transparent_black;

    [[nodiscard]] bool operator==(sampler const&) const = default;
};
} // namespace sg
