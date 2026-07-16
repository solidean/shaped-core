#pragma once

#include <shaped-graphics/raw_texture.hh> // texture_description, texture_dimension

/// The compile-time shape of a texture: `texture_traits<Dim, Array, Cube, Multisampled>` — the single
/// template argument of `texture<Traits>` (texture.hh). It carries the shape as static members, a
/// `matches()` runtime shape check, and the per-view parameter bags the view factories take. Split out so
/// code that only names shapes / params need not pull in the whole wrapper.

namespace sg
{
/// A half-open sub-range selection for one texture-view axis (mips, array slices, cubes, depth slices),
/// in that axis's own units. Default `{}` selects the whole axis; a `count < 0` means "from `start` to
/// the end". The factory resolves and bounds-checks it against the texture's extent.
struct view_range
{
    int start = 0;
    int count = -1; ///< < 0 = to the end of the axis
};

// -- View-factory parameter bags. Each texture shape exposes the ones that make sense for it (as
//    `Traits::read_only_params`, `Traits::read_write_2d_params`, …), so a call names only the axes that
//    exist for that shape and a nonsensical field is a compile error, not a silently-ignored value.

/// Selectable axes are detected structurally by the factories (via `requires { p.field; }`), so these
/// stay plain aggregates. `no_params` is the placeholder a shape uses for a view it does not support.
struct no_params
{
};

// Read-only (sampled / SRV) natural-dimension params.
struct readonly_params
{
    view_range mips;
};
struct readonly_array_params
{
    view_range mips;
    view_range slices; ///< in array-slice units (a cube face is one slice)
};
struct readonly_cube_array_params
{
    view_range mips;
    view_range cubes; ///< in whole-cube units (6 faces each)
};

// Read-only (sampled / SRV) reinterpreting params (bind one slice/face/cube as a lower dimension).
struct readonly_slice_params ///< one slice of a 1D/2D array -> Texture1D/Texture2D
{
    int slice = 0;
    view_range mips;
};
struct readonly_2d_of_cube_params ///< one face of a cube -> Texture2D
{
    int face = 0;
    view_range mips;
};
struct readonly_2d_of_cube_array_params ///< one face of one cube of a cube array -> Texture2D
{
    int cube = 0;
    int face = 0;
    view_range mips;
};
struct readonly_cube_of_array_params ///< one cube of a cube array -> TextureCube
{
    int cube = 0;
    view_range mips;
};
struct readonly_2d_array_of_cube_params ///< a cube's faces as a Texture2DArray (all faces, or a sub-range)
{
    view_range slices; ///< in face/slice units (a cube array is 6 per cube)
    view_range mips;
};

// Read-write (storage / UAV) natural-dimension params. Always a single mip level.
struct readwrite_params
{
    int mip = 0;
};
struct readwrite_array_params
{
    int mip = 0;
    view_range slices; ///< in array-slice units (a cube face is one slice)
};
struct readwrite_3d_params
{
    int mip = 0;
    view_range depth_slices; ///< the 3D texture's W/Z axis (FirstWSlice/WSize)
};

// Read-write (storage / UAV) reinterpreting params.
struct readwrite_slice_params ///< one slice of a 1D/2D array -> Texture1D/Texture2D
{
    int slice = 0;
    int mip = 0;
};
struct readwrite_2d_of_cube_params ///< one face of a cube -> Texture2D
{
    int face = 0;
    int mip = 0;
};
struct readwrite_2d_of_cube_array_params ///< one face of one cube of a cube array -> Texture2D
{
    int cube = 0;
    int face = 0;
    int mip = 0;
};

namespace impl
{
// Each picker maps a compile-time shape to the parameter bag its factory takes; a shape that lacks the
// view returns `no_params`. `texture_traits` turns these into its nested `*_params` aliases.

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_only_params()
{
    if constexpr (!Cube && !Array)
        return readonly_params{}; // plain 1D/2D/3D (and 2D-MS: mips is forced to the single level)
    else if constexpr (Cube && !Array && !MS)
        return readonly_params{}; // a cube samples as one TextureCube (all 6 faces)
    else if constexpr (Cube && Array && !MS)
        return readonly_cube_array_params{};
    else
        return readonly_array_params{}; // arrays, and every MS array-like (incl. MS cubes -> 2D-MS array)
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_write_params()
{
    if constexpr (MS)
        return no_params{}; // MSAA has no UAV
    else if constexpr (Dim == texture_dimension::d3)
        return readwrite_3d_params{};
    else if constexpr (Array || Cube)
        return readwrite_array_params{};
    else
        return readwrite_params{};
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_only_2d_params()
{
    if constexpr (Dim == texture_dimension::d2 && Cube && Array)
        return readonly_2d_of_cube_array_params{};
    else if constexpr (Dim == texture_dimension::d2 && Cube)
        return readonly_2d_of_cube_params{};
    else if constexpr (Dim == texture_dimension::d2 && Array)
        return readonly_slice_params{};
    else
        return no_params{};
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_only_1d_params()
{
    if constexpr (Dim == texture_dimension::d1 && Array)
        return readonly_slice_params{};
    else
        return no_params{};
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_only_cube_params()
{
    if constexpr (Cube && Array && !MS)
        return readonly_cube_of_array_params{};
    else
        return no_params{};
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_only_2d_array_params()
{
    if constexpr (Cube && !MS) // a cube / cube array's faces reinterpreted as a plain 2D array
        return readonly_2d_array_of_cube_params{};
    else
        return no_params{};
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_write_2d_params()
{
    if constexpr (MS)
        return no_params{};
    else if constexpr (Dim == texture_dimension::d2 && Cube && Array)
        return readwrite_2d_of_cube_array_params{};
    else if constexpr (Dim == texture_dimension::d2 && Cube)
        return readwrite_2d_of_cube_params{};
    else if constexpr (Dim == texture_dimension::d2 && Array)
        return readwrite_slice_params{};
    else
        return no_params{};
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_read_write_1d_params()
{
    if constexpr (!MS && Dim == texture_dimension::d1 && Array)
        return readwrite_slice_params{};
    else
        return no_params{};
}

// Render-target / depth-stencil view params. Same axes as a storage view — a single mip plus a slice
// selection — but multisampling is allowed (MSAA render targets / depth are valid). 2D-only.

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_target_view_params()
{
    if constexpr (Dim != texture_dimension::d2)
        return no_params{};
    else if constexpr (Array || Cube)
        return readwrite_array_params{}; // {mip, slices}
    else
        return readwrite_params{}; // {mip}
}

template <texture_dimension Dim, bool Array, bool Cube, bool MS>
consteval auto pick_target_view_2d_params()
{
    if constexpr (Dim != texture_dimension::d2)
        return no_params{};
    else if constexpr (Cube && Array)
        return readwrite_2d_of_cube_array_params{}; // {cube, face, mip}
    else if constexpr (Cube)
        return readwrite_2d_of_cube_params{}; // {face, mip}
    else if constexpr (Array)
        return readwrite_slice_params{}; // {slice, mip}
    else
        return no_params{};
}
} // namespace impl

/// The compile-time shape of a texture — the single template argument of `texture<Traits>`. Carries the
/// shape as static members, a `matches()` runtime shape check against a texture_description, and the
/// per-view parameter bags the factories take (`read_only_params`, `read_write_2d_params`, …). Array-ness
/// / cube-ness / multisampling are the *kind* only; extents / mip count / format stay runtime (on the
/// description). Prefer the `texture_2d` / `texture_cube_array` / … typedefs over spelling this.
template <texture_dimension Dim, bool Array = false, bool Cube = false, bool Multisampled = false>
struct texture_traits
{
    static constexpr texture_dimension dimension = Dim;
    static constexpr bool is_array = Array;
    static constexpr bool is_cube = Cube;
    static constexpr bool is_multisampled = Multisampled;

    /// Whether a runtime description has exactly this shape. Array-ness / cube-ness / multisampling are
    /// derived from the description (`array_layers` set, `is_cube`, `sample_count > 1`), not separate flags.
    [[nodiscard]] static constexpr bool matches(texture_description const& d)
    {
        return d.dimension == Dim && d.array_layers.has_value() == Array && d.is_cube == Cube
            && (d.sample_count > 1) == Multisampled;
    }

    using read_only_params = decltype(impl::pick_read_only_params<Dim, Array, Cube, Multisampled>());
    using read_write_params = decltype(impl::pick_read_write_params<Dim, Array, Cube, Multisampled>());
    using read_only_2d_params = decltype(impl::pick_read_only_2d_params<Dim, Array, Cube, Multisampled>());
    using read_only_1d_params = decltype(impl::pick_read_only_1d_params<Dim, Array, Cube, Multisampled>());
    using read_only_cube_params = decltype(impl::pick_read_only_cube_params<Dim, Array, Cube, Multisampled>());
    using read_only_2d_array_params = decltype(impl::pick_read_only_2d_array_params<Dim, Array, Cube, Multisampled>());
    using read_write_2d_params = decltype(impl::pick_read_write_2d_params<Dim, Array, Cube, Multisampled>());
    using read_write_1d_params = decltype(impl::pick_read_write_1d_params<Dim, Array, Cube, Multisampled>());

    // Render-target / depth-stencil views share the storage-view axes (single mip + slice selection); RTV
    // and DSV use the same bags. `render_target_params` / `render_target_2d_params` name them for the factories.
    using render_target_params = decltype(impl::pick_target_view_params<Dim, Array, Cube, Multisampled>());
    using render_target_2d_params = decltype(impl::pick_target_view_2d_params<Dim, Array, Cube, Multisampled>());
    using depth_stencil_params = render_target_params;
    using depth_stencil_2d_params = render_target_2d_params;
};
} // namespace sg
