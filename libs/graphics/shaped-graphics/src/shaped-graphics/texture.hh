#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/views.hh>

namespace sg
{
/// The compile-time shape of a texture: the axes that are variable across the typedefs below, bundled
/// into one structural value so `texture<Traits>` takes a single template parameter. Extents, mip count,
/// format etc. are runtime (they live on texture_description); only the *kind* of texture is encoded here.
struct texture_traits
{
    texture_dimension dimension = texture_dimension::d2;
    bool is_array = false;
    bool is_cube = false;
    bool is_multisampled = false;

    [[nodiscard]] constexpr bool operator==(texture_traits const&) const = default;
};

/// The compile-time traits of a runtime description — the bridge used to shape-check a raw_texture
/// against a `texture<Traits>` wrapper.
[[nodiscard]] constexpr texture_traits traits_of(texture_description const& d)
{
    return texture_traits{
        .dimension = d.dimension,
        .is_array = d.array_layers.has_value(),
        .is_cube = d.is_cube,
        .is_multisampled = d.sample_count > 1,
    };
}

/// A strongly-typed view onto a raw_texture whose shape is fixed at compile time by `Traits`. Privately
/// holds a raw_texture_handle; adds shape-specific accessors gated by `requires` so, e.g., `depth()`
/// exists only on 3D textures and `array_layers()` only on arrays — misuse is a compile error, not a
/// runtime check. Reach the raw resource for the general (raw) API via `raw()`. Value type: copy is a
/// cheap handle copy. Prefer the typedefs (`texture_2d`, `texture_cube_array`, …) over spelling Traits.
template <texture_traits Traits>
class texture
{
    // Compile-time shape, mirrored from Traits for convenient introspection.
public:
    static constexpr texture_dimension dimension = Traits.dimension;
    static constexpr bool is_array = Traits.is_array;
    static constexpr bool is_cube = Traits.is_cube;
    static constexpr bool is_multisampled = Traits.is_multisampled;

public:
    texture() = default;

    /// Wraps a raw_texture, asserting its runtime shape matches `Traits`. The handle must be non-null.
    explicit texture(raw_texture_handle raw) : _raw(cc::move(raw))
    {
        CC_ASSERT(_raw != nullptr, "texture<Traits> wraps a non-null raw_texture");
        CC_ASSERT(traits_of(_raw->description()) == Traits, "raw_texture shape does not match texture<Traits>");
    }

    /// The underlying raw resource (never null unless default-constructed). Use this to reach the raw
    /// (general) API; there is no implicit conversion to raw_texture_handle.
    [[nodiscard]] raw_texture_handle const& raw() const { return _raw; }

    // Always-available runtime queries.
    [[nodiscard]] pixel_format format() const { return _raw->format(); }
    [[nodiscard]] int mip_levels() const { return _raw->mip_levels(); }
    [[nodiscard]] int width() const { return _raw->width(); }

    // Shape-gated queries — present only where the shape has that axis.
    [[nodiscard]] int height() const
        requires(Traits.dimension != texture_dimension::d1)
    {
        return _raw->height();
    }
    [[nodiscard]] int depth() const
        requires(Traits.dimension == texture_dimension::d3)
    {
        return _raw->depth();
    }
    [[nodiscard]] int array_layers() const
        requires(Traits.is_array)
    {
        return _raw->array_layers();
    }
    [[nodiscard]] int sample_count() const
        requires(Traits.is_multisampled)
    {
        return _raw->sample_count();
    }

    // Shader-facing views — strongly-typed descriptors a binding_group binds. Each asserts the texture
    // carries the matching usage. The view spans the whole texture (a UAV spans a single mip level).

    /// A read-only (sampled / SRV) view over the whole texture. Requires readonly_texture usage.
    [[nodiscard]] texture_readonly_view as_readonly_view() const
    {
        CC_ASSERT(has_flag(_raw->usage(), texture_usage::readonly_texture), "texture lacks readonly_texture usage");
        return texture_readonly_view{.texture = _raw, .format = _raw->format(), .range = whole_range()};
    }

    /// A read-write (storage / UAV) view over one mip level (default 0). Requires readwrite_texture usage
    /// and a non-multisampled texture (D3D12 forbids MSAA UAVs).
    [[nodiscard]] texture_readwrite_view as_readwrite_view(int mip = 0) const
        requires(!Traits.is_multisampled)
    {
        CC_ASSERT(has_flag(_raw->usage(), texture_usage::readwrite_texture), "texture lacks readwrite_texture usage");
        CC_ASSERT(mip >= 0 && mip < _raw->mip_levels(), "readwrite view mip level out of range");
        subresource_range r = whole_range();
        r.mip_range = {.start = mip, .end = mip + 1}; // a UAV targets a single mip level
        return texture_readwrite_view{.texture = _raw, .format = _raw->format(), .range = r};
    }

private:
    // The whole-texture subresource range: all mips, all array slices (a cube is 6 per cube), all aspects.
    [[nodiscard]] subresource_range whole_range() const
    {
        subresource_range r;
        r.mip_range = {.start = 0, .end = _raw->mip_levels()};
        r.array_range = {.start = 0, .end = isize(_raw->array_layers()) * (_raw->is_cube() ? 6 : 1)};
        r.aspect_range = {.start = 0, .end = format_aspect_count(_raw->format())};
        return r;
    }

    raw_texture_handle _raw = nullptr;
};

// Shape typedefs — the ergonomic names. Anything not spelled is defaulted (non-array, non-cube,
// single-sampled).
using texture_1d = texture<texture_traits{.dimension = texture_dimension::d1}>;
using texture_2d = texture<texture_traits{.dimension = texture_dimension::d2}>;
using texture_3d = texture<texture_traits{.dimension = texture_dimension::d3}>;
using texture_cube = texture<texture_traits{.dimension = texture_dimension::d2, .is_cube = true}>;

using texture_1d_array = texture<texture_traits{.dimension = texture_dimension::d1, .is_array = true}>;
using texture_2d_array = texture<texture_traits{.dimension = texture_dimension::d2, .is_array = true}>;
using texture_cube_array = texture<texture_traits{.dimension = texture_dimension::d2, .is_array = true, .is_cube = true}>;

using texture_2d_ms = texture<texture_traits{.dimension = texture_dimension::d2, .is_multisampled = true}>;
using texture_2d_array_ms
    = texture<texture_traits{.dimension = texture_dimension::d2, .is_array = true, .is_multisampled = true}>;
using texture_cube_ms
    = texture<texture_traits{.dimension = texture_dimension::d2, .is_cube = true, .is_multisampled = true}>;
using texture_cube_array_ms
    = texture<texture_traits{.dimension = texture_dimension::d2, .is_array = true, .is_cube = true, .is_multisampled = true}>;
} // namespace sg
