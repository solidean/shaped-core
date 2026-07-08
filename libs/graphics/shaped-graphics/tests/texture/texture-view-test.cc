#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-graphics/views.hh>

#include <memory>

// Pure tests for texture views: the `as_*_view` factories on texture<Traits> build the right strongly-typed
// descriptor, which erases to a `raw_view` with shape `texture`. No GPU — a minimal raw_texture subclass
// (shape/usage metadata only) is enough. The dx12 backend turning these into SRV/UAV descriptors is covered
// in backends/dx12/tests/dx12-texture-view-test.cc.

namespace
{
struct test_texture final : sg::raw_texture
{
    explicit test_texture(sg::texture_description const& d) : sg::raw_texture(d) {}
};

sg::texture_description desc_base(sg::texture_usage usage)
{
    sg::texture_description d;
    d.format = sg::pixel_format::rgba8_unorm;
    d.dimension = sg::texture_dimension::d2;
    d.width = 64;
    d.height = 64;
    d.usage = usage;
    return d;
}

sg::texture_description desc_2d(sg::texture_usage usage, int mips = 1, int samples = 1)
{
    auto d = desc_base(usage);
    d.mip_levels = mips;
    d.sample_count = samples;
    return d;
}

sg::texture_description desc_2d_array(sg::texture_usage usage, int layers, int mips = 1)
{
    auto d = desc_base(usage);
    d.mip_levels = mips;
    d.array_layers = layers;
    return d;
}

sg::texture_description desc_cube(sg::texture_usage usage, int mips = 1)
{
    auto d = desc_base(usage);
    d.mip_levels = mips;
    d.is_cube = true;
    return d;
}

sg::texture_description desc_cube_array(sg::texture_usage usage, int cubes, int mips = 1)
{
    auto d = desc_base(usage);
    d.mip_levels = mips;
    d.is_cube = true;
    d.array_layers = cubes;
    return d;
}

sg::texture_description desc_3d(sg::texture_usage usage, int depth, int mips = 1)
{
    auto d = desc_base(usage);
    d.dimension = sg::texture_dimension::d3;
    d.depth = depth;
    d.mip_levels = mips;
    return d;
}

constexpr cc::start_end one = {.start = 0, .end = 1};

// Which factories a shape exposes — a compile-time contract via `requires` gating. `as_readonly_face_view`
// takes (face) on a plain cube and (cube, face) on a cube array, so its one-arg form gates them apart.
template <class T>
concept has_rw_view = requires(T t) { t.as_readwrite_view(); };
template <class T>
concept has_ro_slice = requires(T t) { t.as_readonly_slice_view(0); };
template <class T>
concept has_rw_slice = requires(T t) { t.as_readwrite_slice_view(0); };
template <class T>
concept has_ro_face = requires(T t) { t.as_readonly_face_view(0); }; // (face) — non-array cube only
template <class T>
concept has_ro_cube = requires(T t) { t.as_readonly_cube_view(0); };
template <class T>
concept has_ro_array_range = requires(T t) { t.as_readonly_view(one); };
template <class T>
concept has_rw_depth_slice = requires(T t) { t.as_readwrite_depth_slice_view(one); };
} // namespace

static_assert(has_rw_view<sg::texture_2d>);
static_assert(!has_rw_view<sg::texture_2d_ms>); // MSAA forbids UAV

// Slice / array-range selection: non-cube array textures only.
static_assert(has_ro_slice<sg::texture_2d_array> && has_ro_array_range<sg::texture_2d_array>);
static_assert(has_rw_slice<sg::texture_2d_array>);
static_assert(!has_ro_slice<sg::texture_2d> && !has_ro_array_range<sg::texture_2d>);
static_assert(!has_rw_slice<sg::texture_2d_array_ms>); // MSAA forbids UAV

// Face / cube selection: cube shapes only, gated by array-ness.
static_assert(has_ro_face<sg::texture_cube> && !has_ro_cube<sg::texture_cube>);
static_assert(has_ro_cube<sg::texture_cube_array> && !has_ro_face<sg::texture_cube_array>); // wants (cube, face)
static_assert(!has_ro_slice<sg::texture_cube_array>);       // cubes are addressed in cube units, not slices
static_assert(!has_ro_array_range<sg::texture_cube_array>); // ditto — use as_readonly_cube_range_view

// Depth-slice selection: 3D storage views only.
static_assert(has_rw_depth_slice<sg::texture_3d>);
static_assert(!has_rw_depth_slice<sg::texture_2d>);

TEST("sg - texture as_readonly_view builds a sampled (SRV) view over the whole texture")
{
    auto const d = desc_2d(sg::texture_usage::readonly_texture, /*mips*/ 2);
    sg::raw_texture_handle raw = std::make_shared<test_texture>(d);
    sg::texture_2d tex(raw);

    sg::raw_view const rv = tex.as_readonly_view().to_raw();
    CHECK(rv.access == sg::view_class::readonly);
    CHECK(rv.shape == sg::view_shape::texture);
    CHECK(rv.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(rv.texture == raw);
    CHECK(rv.buffer == nullptr);
    CHECK(rv.format == sg::pixel_format::rgba8_unorm);
    CHECK(rv.range.mip_range.start == 0);
    CHECK(rv.range.mip_range.end == 2); // all mips
}

TEST("sg - sampled view mip-range selection")
{
    sg::texture_2d tex(std::make_shared<test_texture>(desc_2d(sg::texture_usage::readonly_texture, /*mips*/ 4)));

    auto const from_start = tex.as_readonly_view(/*first_mip*/ 1).to_raw(); // all from start
    CHECK(from_start.range.mip_range.start == 1);
    CHECK(from_start.range.mip_range.end == 4);

    auto const window = tex.as_readonly_view(/*first_mip*/ 1, /*mip_count*/ 2).to_raw();
    CHECK(window.range.mip_range.start == 1);
    CHECK(window.range.mip_range.end == 3);
}

TEST("sg - sampled array views: whole / sub-range / single slice")
{
    sg::texture_2d_array tex(std::make_shared<test_texture>(desc_2d_array(sg::texture_usage::readonly_texture, 6)));

    auto const whole = tex.as_readonly_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::tex_2d_array);
    CHECK(whole.range.array_range.start == 0);
    CHECK(whole.range.array_range.end == 6);

    auto const window = tex.as_readonly_view({.start = 2, .end = 5}).to_raw();
    CHECK(window.view_dimension == sg::texture_view_dimension::tex_2d_array);
    CHECK(window.range.array_range.start == 2);
    CHECK(window.range.array_range.end == 5);

    auto const slice = tex.as_readonly_slice_view(3).to_raw();
    CHECK(slice.view_dimension == sg::texture_view_dimension::tex_2d); // drops to a non-array 2D binding
    CHECK(slice.range.array_range.start == 3);
    CHECK(slice.range.array_range.end == 4);
}

TEST("sg - sampled cube views: whole cube and single face")
{
    sg::texture_cube tex(std::make_shared<test_texture>(desc_cube(sg::texture_usage::readonly_texture)));

    auto const whole = tex.as_readonly_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::cube);
    CHECK(whole.range.array_range.start == 0);
    CHECK(whole.range.array_range.end == 6); // 6 faces

    auto const face = tex.as_readonly_face_view(4).to_raw();
    CHECK(face.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(face.range.array_range.start == 4);
    CHECK(face.range.array_range.end == 5);
}

TEST("sg - sampled cube-array views: whole / cube-range / single cube / single face")
{
    sg::texture_cube_array tex(std::make_shared<test_texture>(desc_cube_array(sg::texture_usage::readonly_texture, 3)));

    auto const whole = tex.as_readonly_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::cube_array);
    CHECK(whole.range.array_range.end == 18); // 3 cubes * 6 faces

    auto const cube_range = tex.as_readonly_cube_range_view({.start = 1, .end = 3}).to_raw();
    CHECK(cube_range.view_dimension == sg::texture_view_dimension::cube_array);
    CHECK(cube_range.range.array_range.start == 6); // cube 1 → face 6
    CHECK(cube_range.range.array_range.end == 18);

    auto const one_cube = tex.as_readonly_cube_view(2).to_raw();
    CHECK(one_cube.view_dimension == sg::texture_view_dimension::cube);
    CHECK(one_cube.range.array_range.start == 12);
    CHECK(one_cube.range.array_range.end == 18);

    auto const face = tex.as_readonly_face_view(/*cube*/ 1, /*face*/ 2).to_raw();
    CHECK(face.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(face.range.array_range.start == 8); // cube 1 face 2 → slice 8
    CHECK(face.range.array_range.end == 9);
}

TEST("sg - as_readwrite_view builds a storage (UAV) view of one mip")
{
    auto const d = desc_2d(sg::texture_usage::readwrite_texture, /*mips*/ 3);
    sg::texture_2d tex(std::make_shared<test_texture>(d));

    sg::raw_view const rv = tex.as_readwrite_view(/*mip*/ 1).to_raw();
    CHECK(rv.access == sg::view_class::readwrite);
    CHECK(rv.shape == sg::view_shape::texture);
    CHECK(rv.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(rv.range.mip_range.start == 1);
    CHECK(rv.range.mip_range.end == 2); // a UAV targets a single mip
}

TEST("sg - storage cube view is a 2D array; single face / slice drop to 2D")
{
    sg::texture_cube cube(std::make_shared<test_texture>(desc_cube(sg::texture_usage::readwrite_texture)));
    auto const whole = cube.as_readwrite_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::tex_2d_array); // no cube UAV
    CHECK(whole.range.array_range.end == 6);

    auto const face = cube.as_readwrite_face_view(2).to_raw();
    CHECK(face.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(face.range.array_range.start == 2);

    sg::texture_2d_array arr(std::make_shared<test_texture>(desc_2d_array(sg::texture_usage::readwrite_texture, 4)));
    auto const slice = arr.as_readwrite_slice_view(3).to_raw();
    CHECK(slice.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(slice.range.array_range.start == 3);
    CHECK(slice.range.array_range.end == 4);
}

TEST("sg - storage 3D view carries a depth-slice window")
{
    sg::texture_3d tex(std::make_shared<test_texture>(desc_3d(sg::texture_usage::readwrite_texture, /*depth*/ 8)));

    auto const whole = tex.as_readwrite_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::tex_3d);
    CHECK(whole.depth_slice_range.start == 0);
    CHECK(whole.depth_slice_range.end == 8); // all depth slices

    auto const window = tex.as_readwrite_depth_slice_view({.start = 2, .end = 5}).to_raw();
    CHECK(window.view_dimension == sg::texture_view_dimension::tex_3d);
    CHECK(window.depth_slice_range.start == 2);
    CHECK(window.depth_slice_range.end == 5);
}

TEST("sg - texture views assert on missing usage")
{
    sg::texture_2d storage_only(std::make_shared<test_texture>(desc_2d(sg::texture_usage::readwrite_texture)));
    CHECK_ASSERTS(storage_only.as_readonly_view()); // lacks readonly_texture

    sg::texture_2d sampled_only(std::make_shared<test_texture>(desc_2d(sg::texture_usage::readonly_texture)));
    CHECK_ASSERTS(sampled_only.as_readwrite_view()); // lacks readwrite_texture
}

TEST("sg - texture views assert on out-of-range selection")
{
    sg::texture_2d_array arr(std::make_shared<test_texture>(desc_2d_array(sg::texture_usage::readonly_texture, 4, 2)));
    CHECK_ASSERTS(arr.as_readonly_slice_view(4));                // slice past the last
    CHECK_ASSERTS(arr.as_readonly_view({.start = 0, .end = 5})); // range past the last
    CHECK_ASSERTS(arr.as_readonly_view(/*first_mip*/ 2));        // mip past the last

    sg::texture_cube cube(std::make_shared<test_texture>(desc_cube(sg::texture_usage::readonly_texture)));
    CHECK_ASSERTS(cube.as_readonly_face_view(6)); // face index out of range
}

TEST("sg - texture binding types accept the matching texture view")
{
    auto const d = desc_2d(sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture);
    sg::texture_2d tex(std::make_shared<test_texture>(d));

    CHECK(sg::accepts(sg::binding_type::sampled_texture, tex.as_readonly_view().to_raw()));
    CHECK(sg::accepts(sg::binding_type::storage_texture, tex.as_readwrite_view().to_raw()));
    CHECK(!sg::accepts(sg::binding_type::sampled_texture, tex.as_readwrite_view().to_raw()));           // wrong access
    CHECK(!sg::accepts(sg::binding_type::readonly_structured_buffer, tex.as_readonly_view().to_raw())); // wrong shape
}
