#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/raw_texture.hh>

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
/// runtime check. Convertible back to raw_texture_handle for the general (raw) API. Value type: copy is
/// a cheap handle copy. Prefer the typedefs (`texture_2d`, `texture_cube_array`, …) over spelling Traits.
template <texture_traits Traits>
class texture
{
public:
    texture() = default;

    /// Wraps a raw_texture, asserting its runtime shape matches `Traits`. The handle must be non-null.
    explicit texture(raw_texture_handle raw) : _raw(cc::move(raw))
    {
        CC_ASSERT(_raw != nullptr, "texture<Traits> wraps a non-null raw_texture");
        CC_ASSERT(traits_of(_raw->description()) == Traits, "raw_texture shape does not match texture<Traits>");
    }

    /// The underlying raw resource (never null unless default-constructed).
    [[nodiscard]] raw_texture_handle const& raw() const { return _raw; }
    operator raw_texture_handle() const { return _raw; }
    [[nodiscard]] explicit operator bool() const { return _raw != nullptr; }

    // Compile-time shape, mirrored from Traits for convenient introspection.
    static constexpr texture_dimension dimension = Traits.dimension;
    static constexpr bool is_array = Traits.is_array;
    static constexpr bool is_cube = Traits.is_cube;
    static constexpr bool is_multisampled = Traits.is_multisampled;

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

private:
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
