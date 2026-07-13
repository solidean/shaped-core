#include <nexus/test.hh>
#include <shaped-graphics/binding.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-graphics/views.hh>

#include <memory>

// Pure tests for texture views: the `as_*_view` factories on texture<Traits> build the right strongly-typed
// descriptor, which erases to a `raw_view` with shape `texture`. Each factory takes a shape-specific
// parameter bag (`Traits::*_params`) naming only the axes that shape has. No GPU — a minimal raw_texture
// subclass (shape/usage metadata only) is enough. The dx12 backend turning these into SRV/UAV descriptors
// is covered in backends/dx12/tests/dx12-texture-view-test.cc.

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

// A 2D texture with a chosen format (for attachment format-validity tests: a depth DSV target, or a
// deliberately wrong format).
sg::texture_description desc_2d_fmt(sg::texture_usage usage, sg::pixel_format format, int mips = 1)
{
    auto d = desc_2d(usage, mips);
    d.format = format;
    return d;
}

// Which view factories a shape exposes, and which axes its params bag names — a compile-time contract.
template <class T>
concept has_rw_view = requires(T t) { t.as_readwrite_view(); };
template <class T>
concept has_ro_2d = requires(T t) { t.as_readonly_2d_view(); };
template <class T>
concept has_ro_1d = requires(T t) { t.as_readonly_1d_view(); };
template <class T>
concept has_ro_cube = requires(T t) { t.as_readonly_cube_view(); };
template <class T>
concept has_ro_2d_array = requires(T t) { t.as_readonly_2d_array_view(); };
template <class T>
concept has_rw_2d = requires(T t) { t.as_readwrite_2d_view(); };
template <class T>
concept ro_has_slices = requires(typename T::read_only_params p) { p.slices; };
template <class T>
concept ro_has_cubes = requires(typename T::read_only_params p) { p.cubes; };
template <class T>
concept rw_has_depth_slices = requires(typename T::read_write_params p) { p.depth_slices; };
template <class T>
concept has_rtv = requires(T t) { t.as_render_target_view(); };
template <class T>
concept has_dsv = requires(T t) { t.as_depth_stencil_view(); };
template <class T>
concept has_rtv_2d = requires(T t) { t.as_render_target_2d_view(); };
template <class T>
concept has_dsv_2d = requires(T t) { t.as_depth_stencil_2d_view(); };
} // namespace

static_assert(has_rw_view<sg::texture_2d>);
static_assert(!has_rw_view<sg::texture_2d_ms>); // MSAA forbids UAV

// Natural-view params carry only the axes the shape has.
static_assert(ro_has_slices<sg::texture_2d_array> && !ro_has_cubes<sg::texture_2d_array>);
static_assert(ro_has_cubes<sg::texture_cube_array> && !ro_has_slices<sg::texture_cube_array>);
static_assert(!ro_has_slices<sg::texture_2d> && !ro_has_cubes<sg::texture_2d>);
static_assert(rw_has_depth_slices<sg::texture_3d> && !rw_has_depth_slices<sg::texture_2d>);

// Reinterpreting views exist only where the shape supports the target dimension.
static_assert(has_ro_2d<sg::texture_2d_array> && has_ro_2d<sg::texture_cube> && has_ro_2d<sg::texture_cube_array>);
static_assert(!has_ro_2d<sg::texture_2d> && !has_ro_2d<sg::texture_3d>);
static_assert(has_ro_1d<sg::texture_1d_array> && !has_ro_1d<sg::texture_2d_array>);
static_assert(has_ro_cube<sg::texture_cube_array> && !has_ro_cube<sg::texture_cube>);
// A cube's faces can also be sampled as a plain 2D array (both cube shapes, non-MS).
static_assert(has_ro_2d_array<sg::texture_cube> && has_ro_2d_array<sg::texture_cube_array>);
static_assert(!has_ro_2d_array<sg::texture_2d_array> && !has_ro_2d_array<sg::texture_cube_ms>);
static_assert(has_rw_2d<sg::texture_2d_array> && has_rw_2d<sg::texture_cube>);
static_assert(!has_rw_2d<sg::texture_2d_array_ms>); // MSAA forbids UAV

// Attachment (RTV/DSV) views exist on every 2D-shaped texture (MSAA and cubes included), but not on 1D/3D.
static_assert(has_rtv<sg::texture_2d> && has_rtv<sg::texture_2d_array> && has_rtv<sg::texture_2d_ms>);
static_assert(has_rtv<sg::texture_cube> && has_rtv<sg::texture_cube_array> && has_rtv<sg::texture_2d_array_ms>);
static_assert(!has_rtv<sg::texture_1d> && !has_rtv<sg::texture_1d_array> && !has_rtv<sg::texture_3d>);
static_assert(has_dsv<sg::texture_2d> && has_dsv<sg::texture_2d_array> && has_dsv<sg::texture_2d_ms>);
static_assert(has_dsv<sg::texture_cube> && !has_dsv<sg::texture_1d> && !has_dsv<sg::texture_3d>);
// The single-slice reinterpret exists only on 2D array / cube shapes (bind one layer/face as a 2D target).
static_assert(has_rtv_2d<sg::texture_2d_array> && has_rtv_2d<sg::texture_cube> && has_dsv_2d<sg::texture_2d_array>);
static_assert(!has_rtv_2d<sg::texture_2d> && !has_rtv_2d<sg::texture_3d> && !has_dsv_2d<sg::texture_2d>);

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

    auto const from_start = tex.as_readonly_view({.mips = {.start = 1}}).to_raw(); // count<0 -> to the end
    CHECK(from_start.range.mip_range.start == 1);
    CHECK(from_start.range.mip_range.end == 4);

    auto const window = tex.as_readonly_view({.mips = {.start = 1, .count = 2}}).to_raw();
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

    auto const window = tex.as_readonly_view({.slices = {.start = 2, .count = 3}}).to_raw();
    CHECK(window.view_dimension == sg::texture_view_dimension::tex_2d_array);
    CHECK(window.range.array_range.start == 2);
    CHECK(window.range.array_range.end == 5);

    auto const slice = tex.as_readonly_2d_view({.slice = 3}).to_raw();
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

    auto const face = tex.as_readonly_2d_view({.face = 4}).to_raw();
    CHECK(face.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(face.range.array_range.start == 4);
    CHECK(face.range.array_range.end == 5);
}

TEST("sg - a cube's faces can be sampled as a plain 2D array")
{
    sg::texture_cube cube(std::make_shared<test_texture>(desc_cube(sg::texture_usage::readonly_texture)));

    auto const whole = cube.as_readonly_2d_array_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::tex_2d_array); // not TextureCube
    CHECK(whole.range.array_range.start == 0);
    CHECK(whole.range.array_range.end == 6); // all 6 faces

    auto const range = cube.as_readonly_2d_array_view({.slices = {.start = 2, .count = 3}}).to_raw();
    CHECK(range.range.array_range.start == 2);
    CHECK(range.range.array_range.end == 5);

    // A cube array's 6*N faces likewise reinterpret as one flat 2D array.
    sg::texture_cube_array arr(std::make_shared<test_texture>(desc_cube_array(sg::texture_usage::readonly_texture, 3)));
    auto const arr_whole = arr.as_readonly_2d_array_view().to_raw();
    CHECK(arr_whole.view_dimension == sg::texture_view_dimension::tex_2d_array);
    CHECK(arr_whole.range.array_range.end == 18); // 3 cubes * 6 faces
}

TEST("sg - sampled cube-array views: whole / cube-range / single cube / single face")
{
    sg::texture_cube_array tex(std::make_shared<test_texture>(desc_cube_array(sg::texture_usage::readonly_texture, 3)));

    auto const whole = tex.as_readonly_view().to_raw();
    CHECK(whole.view_dimension == sg::texture_view_dimension::cube_array);
    CHECK(whole.range.array_range.end == 18); // 3 cubes * 6 faces

    auto const cube_range = tex.as_readonly_view({.cubes = {.start = 1, .count = 2}}).to_raw();
    CHECK(cube_range.view_dimension == sg::texture_view_dimension::cube_array);
    CHECK(cube_range.range.array_range.start == 6); // cube 1 -> face 6
    CHECK(cube_range.range.array_range.end == 18);

    auto const one_cube = tex.as_readonly_cube_view({.cube = 2}).to_raw();
    CHECK(one_cube.view_dimension == sg::texture_view_dimension::cube);
    CHECK(one_cube.range.array_range.start == 12);
    CHECK(one_cube.range.array_range.end == 18);

    auto const face = tex.as_readonly_2d_view({.cube = 1, .face = 2}).to_raw();
    CHECK(face.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(face.range.array_range.start == 8); // cube 1 face 2 -> slice 8
    CHECK(face.range.array_range.end == 9);
}

TEST("sg - as_readwrite_view builds a storage (UAV) view of one mip")
{
    auto const d = desc_2d(sg::texture_usage::readwrite_texture, /*mips*/ 3);
    sg::texture_2d tex(std::make_shared<test_texture>(d));

    sg::raw_view const rv = tex.as_readwrite_view({.mip = 1}).to_raw();
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

    auto const face = cube.as_readwrite_2d_view({.face = 2}).to_raw();
    CHECK(face.view_dimension == sg::texture_view_dimension::tex_2d);
    CHECK(face.range.array_range.start == 2);

    sg::texture_2d_array arr(std::make_shared<test_texture>(desc_2d_array(sg::texture_usage::readwrite_texture, 4)));
    auto const slice = arr.as_readwrite_2d_view({.slice = 3}).to_raw();
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

    auto const window = tex.as_readwrite_view({.depth_slices = {.start = 2, .count = 3}}).to_raw();
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
    CHECK_ASSERTS(arr.as_readonly_2d_view({.slice = 4}));                      // slice past the last
    CHECK_ASSERTS(arr.as_readonly_view({.slices = {.start = 0, .count = 5}})); // range past the last
    CHECK_ASSERTS(arr.as_readonly_view({.mips = {.start = 2}}));               // mip past the last

    sg::texture_cube cube(std::make_shared<test_texture>(desc_cube(sg::texture_usage::readonly_texture)));
    CHECK_ASSERTS(cube.as_readonly_2d_view({.face = 6})); // face index out of range
}

TEST("sg - texture binding types accept the matching texture view")
{
    auto const d = desc_2d(sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture);
    sg::texture_2d tex(std::make_shared<test_texture>(d));

    CHECK(sg::accepts(sg::binding_type::readonly_texture, tex.as_readonly_view().to_raw()));
    CHECK(sg::accepts(sg::binding_type::readwrite_texture, tex.as_readwrite_view().to_raw()));
    CHECK(!sg::accepts(sg::binding_type::readonly_texture, tex.as_readwrite_view().to_raw()));          // wrong access
    CHECK(!sg::accepts(sg::binding_type::readonly_structured_buffer, tex.as_readonly_view().to_raw())); // wrong shape
}

// -- Attachment views (render_target / depth_stencil). These do not erase to raw_view; their getters ARE
//    the surface, so the tests read them directly.

TEST("sg - as_render_target_view builds a single-mip color-attachment view; getters report size / format")
{
    sg::texture_2d tex(std::make_shared<test_texture>(desc_2d(sg::texture_usage::render_target, /*mips*/ 3)));

    auto const rtv = tex.as_render_target_view({.mip = 1});
    CHECK(rtv.dimension() == sg::texture_view_dimension::tex_2d);
    CHECK(rtv.format() == sg::pixel_format::rgba8_unorm);
    CHECK(rtv.texture() == tex.raw());
    CHECK(rtv.range().mip_range.start == 1);
    CHECK(rtv.range().mip_range.end == 2); // an attachment targets a single mip
    CHECK(rtv.width() == 32);              // 64 >> mip 1
    CHECK(rtv.height() == 32);
}

TEST("sg - as_render_target_view over an array selects the whole slice range; a slice reinterprets as 2D")
{
    sg::texture_2d_array tex(std::make_shared<test_texture>(desc_2d_array(sg::texture_usage::render_target, 4)));

    auto const whole = tex.as_render_target_view();
    CHECK(whole.dimension() == sg::texture_view_dimension::tex_2d_array);
    CHECK(whole.range().array_range.start == 0);
    CHECK(whole.range().array_range.end == 4);

    auto const slice = tex.as_render_target_2d_view({.slice = 2});
    CHECK(slice.dimension() == sg::texture_view_dimension::tex_2d);
    CHECK(slice.range().array_range.start == 2);
    CHECK(slice.range().array_range.end == 3);
    CHECK(slice.width() == 64); // mip 0
}

TEST("sg - a multisampled render target binds as Texture2DMS")
{
    sg::texture_2d_ms tex(
        std::make_shared<test_texture>(desc_2d(sg::texture_usage::render_target, /*mips*/ 1, /*samples*/ 4)));

    auto const rtv = tex.as_render_target_view();
    CHECK(rtv.dimension() == sg::texture_view_dimension::tex_2d_ms);
    CHECK(rtv.range().mip_range.end == 1);
}

TEST("sg - as_depth_stencil_view requires a depth format and covers its aspects")
{
    sg::texture_2d tex(
        std::make_shared<test_texture>(desc_2d_fmt(sg::texture_usage::depth_stencil, sg::pixel_format::depth32_float)));

    auto const dsv = tex.as_depth_stencil_view();
    CHECK(dsv.dimension() == sg::texture_view_dimension::tex_2d);
    CHECK(dsv.format() == sg::pixel_format::depth32_float);
    CHECK(dsv.range().aspect_range.start == 0);
    CHECK(dsv.range().aspect_range.end == 1); // depth-only: one aspect plane

    // A combined depth+stencil format exposes two aspect planes.
    sg::texture_2d ds(std::make_shared<test_texture>(
        desc_2d_fmt(sg::texture_usage::depth_stencil, sg::pixel_format::depth32_float_stencil8)));
    CHECK(ds.as_depth_stencil_view().range().aspect_range.end == 2);
}

TEST("sg - attachment views assert on missing usage")
{
    sg::texture_2d no_rt(std::make_shared<test_texture>(desc_2d(sg::texture_usage::readonly_texture)));
    CHECK_ASSERTS(no_rt.as_render_target_view()); // lacks render_target usage

    sg::texture_2d no_ds(std::make_shared<test_texture>(
        desc_2d_fmt(sg::texture_usage::readonly_texture, sg::pixel_format::depth32_float)));
    CHECK_ASSERTS(no_ds.as_depth_stencil_view()); // lacks depth_stencil usage
}

TEST("sg - attachment views assert on an incompatible format")
{
    // A render target must be a renderable color format, not a depth format.
    sg::texture_2d rt_depth(
        std::make_shared<test_texture>(desc_2d_fmt(sg::texture_usage::render_target, sg::pixel_format::depth32_float)));
    CHECK_ASSERTS(rt_depth.as_render_target_view());

    // A depth-stencil target must be a depth format, not a color format.
    sg::texture_2d ds_color(std::make_shared<test_texture>(desc_2d(sg::texture_usage::depth_stencil)));
    CHECK_ASSERTS(ds_color.as_depth_stencil_view());
}

TEST("sg - attachment views assert on out-of-range selection")
{
    sg::texture_2d_array arr(std::make_shared<test_texture>(desc_2d_array(sg::texture_usage::render_target, 4, 2)));
    CHECK_ASSERTS(arr.as_render_target_2d_view({.slice = 4}));                      // slice past the last
    CHECK_ASSERTS(arr.as_render_target_view({.slices = {.start = 0, .count = 5}})); // range past the last
    CHECK_ASSERTS(arr.as_render_target_view({.mip = 2}));                           // mip past the last
}
