#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/views.hh>

namespace sg
{
/// Sentinel `mip_count` for the sampled-view factories: "all remaining mips from the start index". A
/// `mip_count >= 0` selects exactly that many mips instead.
inline constexpr int all_mips = -1;

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
    // carries the matching usage. A sampled view spans a mip range; a storage view is a single mip level.
    // `dimension` is a reinterpretation (single slice → 2D, single face → 2D, single cube → cube), so
    // selecting one slice is a *different binding* than a size-1 array window.

    // -- Sampled (read-only / SRV) views. `first_mip`/`mip_count` narrow the mip range; the default is all
    //    mips, and a `mip_count < 0` (all_mips) means "to the last mip".

    /// Sampled view over the whole texture in its natural dimension. Requires readonly_texture usage.
    [[nodiscard]] texture_readonly_view as_readonly_view(int first_mip = 0, int mip_count = all_mips) const
    {
        return _readonly(_srv_whole_dim(), _whole_array_range(), _resolve_mips(first_mip, mip_count));
    }

    /// Sampled view over a sub-range of array slices (in slice units), keeping the array dimension. Only on
    /// non-cube array textures.
    [[nodiscard]] texture_readonly_view as_readonly_view(cc::start_end array_range,
                                                         int first_mip = 0,
                                                         int mip_count = all_mips) const
        requires(Traits.is_array && !Traits.is_cube)
    {
        return _readonly(_srv_whole_dim(), _checked_array(array_range), _resolve_mips(first_mip, mip_count));
    }

    /// Sampled view of a single array slice, bound as a non-array texture (Texture1D/Texture2D). Only on
    /// non-cube array textures.
    [[nodiscard]] texture_readonly_view as_readonly_slice_view(int slice, int first_mip = 0, int mip_count = all_mips) const
        requires(Traits.is_array && !Traits.is_cube)
    {
        return _readonly(_element_dim(), _single(slice), _resolve_mips(first_mip, mip_count));
    }

    /// Sampled view of a single cube face, bound as a Texture2D. Only on (non-array) cube textures; `face`
    /// in [0, 6).
    [[nodiscard]] texture_readonly_view as_readonly_face_view(int face, int first_mip = 0, int mip_count = all_mips) const
        requires(Traits.is_cube && !Traits.is_array)
    {
        CC_ASSERT(face >= 0 && face < 6, "cube face index out of range");
        return _readonly(_element_dim(), _single(face), _resolve_mips(first_mip, mip_count));
    }

    /// Sampled view of a single cube (all 6 faces) of a cube array, bound as a TextureCube. Only on cube
    /// arrays.
    [[nodiscard]] texture_readonly_view as_readonly_cube_view(int cube, int first_mip = 0, int mip_count = all_mips) const
        requires(Traits.is_cube && Traits.is_array)
    {
        CC_ASSERT(cube >= 0 && cube < _raw->array_layers(), "cube index out of range");
        return _readonly(texture_view_dimension::cube, {.start = isize(cube) * 6, .end = isize(cube) * 6 + 6},
                         _resolve_mips(first_mip, mip_count));
    }

    /// Sampled view over a sub-range of cubes (in cube units), bound as a TextureCubeArray. Only on cube
    /// arrays.
    [[nodiscard]] texture_readonly_view as_readonly_cube_range_view(cc::start_end cube_range,
                                                                    int first_mip = 0,
                                                                    int mip_count = all_mips) const
        requires(Traits.is_cube && Traits.is_array)
    {
        CC_ASSERT(cube_range.start >= 0 && cube_range.end > cube_range.start && cube_range.end <= _raw->array_layers(),
                  "cube range out of range");
        return _readonly(texture_view_dimension::cube_array, {.start = cube_range.start * 6, .end = cube_range.end * 6},
                         _resolve_mips(first_mip, mip_count));
    }

    /// Sampled view of a single face of a single cube of a cube array, bound as a Texture2D. Only on cube
    /// arrays; `face` in [0, 6).
    [[nodiscard]] texture_readonly_view as_readonly_face_view(int cube,
                                                              int face,
                                                              int first_mip = 0,
                                                              int mip_count = all_mips) const
        requires(Traits.is_cube && Traits.is_array)
    {
        CC_ASSERT(cube >= 0 && cube < _raw->array_layers(), "cube index out of range");
        CC_ASSERT(face >= 0 && face < 6, "cube face index out of range");
        return _readonly(_element_dim(), _single(cube * 6 + face), _resolve_mips(first_mip, mip_count));
    }

    // -- Storage (read-write / UAV) views. Always a single mip level (a UAV targets one mip); requires
    //    readwrite_texture usage and a non-multisampled texture (D3D12 forbids MSAA UAVs, and a cube UAV is
    //    a 2D array).

    /// Storage view over the whole texture at one mip level. Cube/array targets all slices; 3D all depth.
    [[nodiscard]] texture_readwrite_view as_readwrite_view(int mip = 0) const
        requires(!Traits.is_multisampled)
    {
        return _readwrite(_uav_whole_dim(), _whole_array_range(), mip, _whole_depth_slice_range());
    }

    /// Storage view over a sub-range of array slices (in slice units) at one mip. Only on array textures.
    [[nodiscard]] texture_readwrite_view as_readwrite_view(int mip, cc::start_end array_range) const
        requires(!Traits.is_multisampled && Traits.is_array)
    {
        return _readwrite(_uav_whole_dim(), _checked_array(array_range), mip, _whole_depth_slice_range());
    }

    /// Storage view of a single array slice, bound as a non-array texture. Only on array textures.
    [[nodiscard]] texture_readwrite_view as_readwrite_slice_view(int slice, int mip = 0) const
        requires(!Traits.is_multisampled && Traits.is_array)
    {
        return _readwrite(_element_dim(), _single(slice), mip, _whole_depth_slice_range());
    }

    /// Storage view of a single cube face, bound as a Texture2D. Only on (non-array) cube textures.
    [[nodiscard]] texture_readwrite_view as_readwrite_face_view(int face, int mip = 0) const
        requires(!Traits.is_multisampled && Traits.is_cube && !Traits.is_array)
    {
        CC_ASSERT(face >= 0 && face < 6, "cube face index out of range");
        return _readwrite(_element_dim(), _single(face), mip, _whole_depth_slice_range());
    }

    /// Storage view of a single face of one cube of a cube array, bound as a Texture2D. Only on cube arrays.
    [[nodiscard]] texture_readwrite_view as_readwrite_face_view(int cube, int face, int mip = 0) const
        requires(!Traits.is_multisampled && Traits.is_cube && Traits.is_array)
    {
        CC_ASSERT(cube >= 0 && cube < _raw->array_layers(), "cube index out of range");
        CC_ASSERT(face >= 0 && face < 6, "cube face index out of range");
        return _readwrite(_element_dim(), _single(cube * 6 + face), mip, _whole_depth_slice_range());
    }

    /// Storage view over a sub-range of depth slices (the 3D texture's W/Z axis) at one mip. Only on 3D
    /// textures; `depth_slice_range` is half-open, in slices.
    [[nodiscard]] texture_readwrite_view as_readwrite_depth_slice_view(cc::start_end depth_slice_range, int mip = 0) const
        requires(!Traits.is_multisampled && Traits.dimension == texture_dimension::d3)
    {
        CC_ASSERT(depth_slice_range.start >= 0 && depth_slice_range.end > depth_slice_range.start
                      && depth_slice_range.end <= _raw->depth(),
                  "depth slice range out of range");
        return _readwrite(texture_view_dimension::tex_3d, _whole_array_range(), mip, depth_slice_range);
    }

private:
    // The shader-facing dimension a whole-texture sampled view binds as, from the compile-time shape.
    [[nodiscard]] static constexpr texture_view_dimension _srv_whole_dim()
    {
        using d = texture_view_dimension;
        if constexpr (Traits.dimension == texture_dimension::d1)
            return Traits.is_array ? d::tex_1d_array : d::tex_1d;
        else if constexpr (Traits.dimension == texture_dimension::d3)
            return d::tex_3d;
        else if constexpr (Traits.is_multisampled) // d2, no TextureCubeMS: a cube samples as a 2D MS array
            return (Traits.is_array || Traits.is_cube) ? d::tex_2d_ms_array : d::tex_2d_ms;
        else if constexpr (Traits.is_cube)
            return Traits.is_array ? d::cube_array : d::cube;
        else
            return Traits.is_array ? d::tex_2d_array : d::tex_2d;
    }

    // The dimension a whole-texture storage view binds as: no cube / no MSAA — a cube UAV is a 2D array.
    [[nodiscard]] static constexpr texture_view_dimension _uav_whole_dim()
    {
        using d = texture_view_dimension;
        if constexpr (Traits.dimension == texture_dimension::d1)
            return Traits.is_array ? d::tex_1d_array : d::tex_1d;
        else if constexpr (Traits.dimension == texture_dimension::d3)
            return d::tex_3d;
        else // d2, incl. cube (a 2D array of 6 faces)
            return (Traits.is_array || Traits.is_cube) ? d::tex_2d_array : d::tex_2d;
    }

    // The dimension a single-slice / single-face view drops to: the array-less element form.
    [[nodiscard]] static constexpr texture_view_dimension _element_dim()
    {
        using d = texture_view_dimension;
        if constexpr (Traits.dimension == texture_dimension::d1)
            return d::tex_1d;
        else
            return Traits.is_multisampled ? d::tex_2d_ms : d::tex_2d;
    }

    // The whole array-slice range in slice units (a cube is 6 slices per cube).
    [[nodiscard]] cc::start_end _whole_array_range() const
    {
        return {.start = 0, .end = isize(_raw->array_layers()) * (_raw->is_cube() ? 6 : 1)};
    }
    // The whole depth-slice (W/Z) range of a 3D texture; degenerate for other shapes (unused there).
    [[nodiscard]] cc::start_end _whole_depth_slice_range() const
    {
        if constexpr (Traits.dimension == texture_dimension::d3)
            return {.start = 0, .end = _raw->depth()};
        else
            return {.start = 0, .end = 0};
    }
    // A single array slice, bounds-checked against the whole array range.
    [[nodiscard]] cc::start_end _single(int slice) const
    {
        CC_ASSERT(slice >= 0 && slice < _whole_array_range().end, "array slice out of range");
        return {.start = slice, .end = slice + 1};
    }
    // An array sub-range (slice units), bounds-checked against the whole array range.
    [[nodiscard]] cc::start_end _checked_array(cc::start_end r) const
    {
        CC_ASSERT(r.start >= 0 && r.end > r.start && r.end <= _whole_array_range().end, "array range out of range");
        return r;
    }
    // Resolve first_mip/mip_count (all_mips = to the last mip) to a checked half-open mip range.
    [[nodiscard]] cc::start_end _resolve_mips(int first_mip, int mip_count) const
    {
        int const total = _raw->mip_levels();
        CC_ASSERT(first_mip >= 0 && first_mip < total, "mip range start out of range");
        int const end = mip_count < 0 ? total : first_mip + mip_count;
        CC_ASSERT(end > first_mip && end <= total, "mip range out of range");
        return {.start = first_mip, .end = end};
    }

    [[nodiscard]] texture_readonly_view _readonly(texture_view_dimension dim,
                                                  cc::start_end array_range,
                                                  cc::start_end mip_range) const
    {
        CC_ASSERT(has_flag(_raw->usage(), texture_usage::readonly_texture), "texture lacks readonly_texture usage");
        subresource_range r;
        r.mip_range = mip_range;
        r.array_range = array_range;
        r.aspect_range = {.start = 0, .end = format_aspect_count(_raw->format())};
        return texture_readonly_view{.texture = _raw, .dimension = dim, .format = _raw->format(), .range = r};
    }

    [[nodiscard]] texture_readwrite_view _readwrite(texture_view_dimension dim,
                                                    cc::start_end array_range,
                                                    int mip,
                                                    cc::start_end depth_slice_range) const
    {
        CC_ASSERT(has_flag(_raw->usage(), texture_usage::readwrite_texture), "texture lacks readwrite_texture usage");
        CC_ASSERT(mip >= 0 && mip < _raw->mip_levels(), "readwrite view mip level out of range");
        subresource_range r;
        r.mip_range = {.start = mip, .end = mip + 1}; // a UAV targets a single mip level
        r.array_range = array_range;
        r.aspect_range = {.start = 0, .end = format_aspect_count(_raw->format())};
        return texture_readwrite_view{.texture = _raw,
                                      .dimension = dim,
                                      .format = _raw->format(),
                                      .range = r,
                                      .depth_slice_range = depth_slice_range};
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
