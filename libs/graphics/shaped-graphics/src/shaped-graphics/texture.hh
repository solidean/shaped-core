#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::start_end
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/texture_traits.hh> // texture_traits<…> + the view-factory parameter bags
#include <shaped-graphics/views.hh>

namespace sg
{
/// A strongly-typed view onto a raw_texture whose shape is fixed at compile time by `Traits` (a
/// `texture_traits`). Privately holds a raw_texture_handle; adds shape-specific accessors gated by
/// `requires` so, e.g., `depth()` exists only on 3D textures — misuse is a compile error, not a runtime
/// check. Reach the raw resource for the general (raw) API via `raw()`. Value type: copy is a cheap
/// handle copy. Prefer the typedefs (`texture_2d`, `texture_cube_array`, …) over spelling Traits.
template <class Traits>
class texture
{
public:
    // Compile-time shape, mirrored from Traits for convenient introspection.
    static constexpr texture_dimension dimension = Traits::dimension;
    static constexpr bool is_array = Traits::is_array;
    static constexpr bool is_cube = Traits::is_cube;
    static constexpr bool is_multisampled = Traits::is_multisampled;

    // The parameter bag each view factory takes, surfaced for call sites and introspection.
    using read_only_params = typename Traits::read_only_params;
    using read_write_params = typename Traits::read_write_params;
    using read_only_2d_params = typename Traits::read_only_2d_params;
    using read_only_1d_params = typename Traits::read_only_1d_params;
    using read_only_cube_params = typename Traits::read_only_cube_params;
    using read_only_2d_array_params = typename Traits::read_only_2d_array_params;
    using read_write_2d_params = typename Traits::read_write_2d_params;
    using read_write_1d_params = typename Traits::read_write_1d_params;

    texture() = default;

    /// Wraps a raw_texture, asserting its runtime shape matches `Traits`. The handle must be non-null.
    explicit texture(raw_texture_handle raw) : _raw(cc::move(raw))
    {
        CC_ASSERT(_raw != nullptr, "texture<Traits> wraps a non-null raw_texture");
        CC_ASSERT(Traits::matches(_raw->description()), "raw_texture shape does not match texture<Traits>");
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
        requires(Traits::dimension != texture_dimension::d1)
    {
        return _raw->height();
    }
    [[nodiscard]] int depth() const
        requires(Traits::dimension == texture_dimension::d3)
    {
        return _raw->depth();
    }
    [[nodiscard]] int array_layers() const
        requires(Traits::is_array)
    {
        return _raw->array_layers();
    }
    [[nodiscard]] int sample_count() const
        requires(Traits::is_multisampled)
    {
        return _raw->sample_count();
    }

    // Shader-facing views. `as_readonly_view` / `as_readwrite_view` are the natural views (the texture's
    // own dimension); the `_2d` / `_1d` / `cube` variants reinterpret one slice / face / cube as a lower
    // dimension, so binding one slice is a *different binding* than a size-1 array window. Each takes a
    // shape-specific parameter bag (the `*_params` typedefs above) naming only the axes that shape has,
    // and asserts the matching texture_usage.

    /// Sampled (SRV) view over the whole texture in its natural dimension.
    [[nodiscard]] texture_readonly_view as_readonly_view(read_only_params const& params = {}) const
    {
        return _make_readonly(_srv_whole_dim(), _natural_array_range(params), _mips(params));
    }

    /// Sampled view of one array slice, bound as a Texture2D (or Texture2DMS). Only on 2D array shapes.
    [[nodiscard]] texture_readonly_view as_readonly_2d_view(read_only_2d_params const& params = {}) const
        requires(Traits::dimension == texture_dimension::d2 && (Traits::is_array || Traits::is_cube))
    {
        auto const dim = Traits::is_multisampled ? texture_view_dimension::tex_2d_ms : texture_view_dimension::tex_2d;
        return _make_readonly(dim, _single(_pick_slice(params)), _mips(params));
    }

    /// Sampled view of one array slice, bound as a Texture1D. Only on 1D array textures.
    [[nodiscard]] texture_readonly_view as_readonly_1d_view(read_only_1d_params const& params = {}) const
        requires(Traits::dimension == texture_dimension::d1 && Traits::is_array)
    {
        return _make_readonly(texture_view_dimension::tex_1d, _single(_pick_slice(params)), _mips(params));
    }

    /// Sampled view of one cube (all 6 faces) of a cube array, bound as a TextureCube. Only on cube arrays.
    [[nodiscard]] texture_readonly_view as_readonly_cube_view(read_only_cube_params const& params = {}) const
        requires(Traits::is_cube && Traits::is_array && !Traits::is_multisampled)
    {
        CC_ASSERT(params.cube >= 0 && params.cube < _raw->array_layers(), "cube index out of range");
        return _make_readonly(texture_view_dimension::cube,
                              {.start = isize(params.cube) * 6, .end = isize(params.cube) * 6 + 6}, _mips(params));
    }

    /// Sampled view of a cube's faces as a plain Texture2DArray (all faces, or a slice sub-range). Only on
    /// cubes — the alternative to the natural TextureCube view.
    [[nodiscard]] texture_readonly_view as_readonly_2d_array_view(read_only_2d_array_params const& params = {}) const
        requires(Traits::is_cube && !Traits::is_multisampled)
    {
        return _make_readonly(texture_view_dimension::tex_2d_array, _natural_array_range(params), _mips(params));
    }

    /// Storage (UAV) view over the whole texture at one mip level. Not on multisampled textures.
    [[nodiscard]] texture_readwrite_view as_readwrite_view(read_write_params const& params = {}) const
        requires(!Traits::is_multisampled)
    {
        return _make_readwrite(_uav_whole_dim(), _natural_array_range(params), params.mip, _depth_slices(params));
    }

    /// Storage view of one array slice / cube face, bound as a Texture2D. Only on non-MS 2D array shapes.
    [[nodiscard]] texture_readwrite_view as_readwrite_2d_view(read_write_2d_params const& params = {}) const
        requires(!Traits::is_multisampled && Traits::dimension == texture_dimension::d2
                 && (Traits::is_array || Traits::is_cube))
    {
        return _make_readwrite(texture_view_dimension::tex_2d, _single(_pick_slice(params)), params.mip,
                               _whole_depth_slice_range());
    }

    /// Storage view of one array slice, bound as a Texture1D. Only on non-MS 1D array textures.
    [[nodiscard]] texture_readwrite_view as_readwrite_1d_view(read_write_1d_params const& params = {}) const
        requires(!Traits::is_multisampled && Traits::dimension == texture_dimension::d1 && Traits::is_array)
    {
        return _make_readwrite(texture_view_dimension::tex_1d, _single(_pick_slice(params)), params.mip,
                               _whole_depth_slice_range());
    }

private:
    // -- Dimension mapping (compile-time from the shape). --

    // The dimension a whole-texture sampled view binds as.
    [[nodiscard]] static constexpr texture_view_dimension _srv_whole_dim()
    {
        using d = texture_view_dimension;
        if constexpr (Traits::dimension == texture_dimension::d1)
            return Traits::is_array ? d::tex_1d_array : d::tex_1d;
        else if constexpr (Traits::dimension == texture_dimension::d3)
            return d::tex_3d;
        else if constexpr (Traits::is_multisampled) // d2, no TextureCubeMS: a cube samples as a 2D-MS array
            return (Traits::is_array || Traits::is_cube) ? d::tex_2d_ms_array : d::tex_2d_ms;
        else if constexpr (Traits::is_cube)
            return Traits::is_array ? d::cube_array : d::cube;
        else
            return Traits::is_array ? d::tex_2d_array : d::tex_2d;
    }

    // The dimension a whole-texture storage view binds as: no cube / no MSAA — a cube UAV is a 2D array.
    [[nodiscard]] static constexpr texture_view_dimension _uav_whole_dim()
    {
        using d = texture_view_dimension;
        if constexpr (Traits::dimension == texture_dimension::d1)
            return Traits::is_array ? d::tex_1d_array : d::tex_1d;
        else if constexpr (Traits::dimension == texture_dimension::d3)
            return d::tex_3d;
        else // d2, incl. cube (a 2D array of 6 faces)
            return (Traits::is_array || Traits::is_cube) ? d::tex_2d_array : d::tex_2d;
    }

    // -- Range resolution from parameter bags (fields are detected structurally). --

    // The whole array-slice count in slice units (a cube is 6 slices per cube).
    [[nodiscard]] int _whole_slice_count() const { return _raw->array_layers() * (Traits::is_cube ? 6 : 1); }
    [[nodiscard]] cc::start_end _whole_array_range() const { return {.start = 0, .end = _whole_slice_count()}; }
    [[nodiscard]] cc::start_end _whole_depth_slice_range() const
    {
        if constexpr (Traits::dimension == texture_dimension::d3)
            return {.start = 0, .end = _raw->depth()};
        else
            return {.start = 0, .end = 0};
    }

    // Resolve a view_range against an axis total (count < 0 = to the end), bounds-checked.
    [[nodiscard]] cc::start_end _resolve(view_range sel, int total) const
    {
        CC_ASSERT(sel.start >= 0 && sel.start < total, "view range start out of range");
        int const end = sel.count < 0 ? total : sel.start + sel.count;
        CC_ASSERT(end > sel.start && end <= total, "view range out of range");
        return {.start = sel.start, .end = end};
    }
    // A single array slice, bounds-checked against the whole array range.
    [[nodiscard]] cc::start_end _single(int slice) const
    {
        CC_ASSERT(slice >= 0 && slice < _whole_slice_count(), "array slice out of range");
        return {.start = slice, .end = slice + 1};
    }

    // The mip range a params bag selects: `p.mips` if present, else the whole range. MSAA has one mip.
    template <class P>
    [[nodiscard]] cc::start_end _mips(P const& p) const
    {
        if constexpr (requires { p.mips; })
            return _resolve(p.mips, _raw->mip_levels());
        else
            return {.start = 0, .end = _raw->mip_levels()};
    }
    // The natural-view array range a params bag selects (cubes in cube units, slices in slice units).
    template <class P>
    [[nodiscard]] cc::start_end _natural_array_range(P const& p) const
    {
        if constexpr (requires { p.cubes; })
        {
            auto const c = _resolve(p.cubes, _raw->array_layers());
            return {.start = c.start * 6, .end = c.end * 6};
        }
        else if constexpr (requires { p.slices; })
            return _resolve(p.slices, _whole_slice_count());
        else
            return _whole_array_range();
    }
    // The depth (W/Z) slice range a 3D storage params bag selects, else the whole depth.
    template <class P>
    [[nodiscard]] cc::start_end _depth_slices(P const& p) const
    {
        if constexpr (requires { p.depth_slices; })
            return _resolve(p.depth_slices, _raw->depth());
        else
            return _whole_depth_slice_range();
    }
    // The single slice a reinterpret params bag names: a plain slice, a cube face, or (cube, face).
    template <class P>
    [[nodiscard]] int _pick_slice(P const& p) const
    {
        if constexpr (requires { p.cube; })
        {
            CC_ASSERT(p.cube >= 0 && p.cube < _raw->array_layers(), "cube index out of range");
            CC_ASSERT(p.face >= 0 && p.face < 6, "cube face index out of range");
            return p.cube * 6 + p.face;
        }
        else if constexpr (requires { p.face; })
        {
            CC_ASSERT(p.face >= 0 && p.face < 6, "cube face index out of range");
            return p.face;
        }
        else
            return p.slice;
    }

    // -- View assembly (assert usage, fill the subresource range). --

    [[nodiscard]] texture_readonly_view _make_readonly(texture_view_dimension dim,
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

    [[nodiscard]] texture_readwrite_view _make_readwrite(texture_view_dimension dim,
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
using texture_1d = texture<texture_traits<texture_dimension::d1>>;
using texture_2d = texture<texture_traits<texture_dimension::d2>>;
using texture_3d = texture<texture_traits<texture_dimension::d3>>;
using texture_cube = texture<texture_traits<texture_dimension::d2, /*array*/ false, /*cube*/ true>>;

using texture_1d_array = texture<texture_traits<texture_dimension::d1, true>>;
using texture_2d_array = texture<texture_traits<texture_dimension::d2, true>>;
using texture_cube_array = texture<texture_traits<texture_dimension::d2, true, true>>;

using texture_2d_ms = texture<texture_traits<texture_dimension::d2, false, false, true>>;
using texture_2d_array_ms = texture<texture_traits<texture_dimension::d2, true, false, true>>;
using texture_cube_ms = texture<texture_traits<texture_dimension::d2, false, true, true>>;
using texture_cube_array_ms = texture<texture_traits<texture_dimension::d2, true, true, true>>;
} // namespace sg
