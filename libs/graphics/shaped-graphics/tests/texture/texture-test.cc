#include <nexus/test.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/texture.hh>

#include <memory>

// Pure value/type tests: pixel_format helpers, texture_description shape, and the concept-gated
// texture<Traits> surface — no GPU needed. Real texture creation lives in texture-create-test.cc
// (INVOCABLE_TESTs run against each backend).

// --- pixel_format helpers ----------------------------------------------------------------------------

TEST("sg - pixel_format classification")
{
    using pf = sg::pixel_format;

    CHECK(sg::is_depth_format(pf::depth16_unorm));
    CHECK(sg::is_depth_format(pf::depth32_float));
    CHECK(sg::is_depth_format(pf::depth32_float_stencil8));
    CHECK(!sg::is_depth_format(pf::rgba8_unorm));

    CHECK(sg::has_stencil(pf::depth32_float_stencil8));
    CHECK(!sg::has_stencil(pf::depth32_float));
    CHECK(sg::is_depth_stencil_format(pf::depth32_float_stencil8));
    CHECK(!sg::is_depth_stencil_format(pf::depth32_float));

    CHECK(sg::is_compressed_format(pf::bc1_rgba_unorm));
    CHECK(sg::is_compressed_format(pf::bc7_rgba_unorm_srgb));
    CHECK(!sg::is_compressed_format(pf::rgba8_unorm));
    CHECK(!sg::is_compressed_format(pf::depth32_float));
}

TEST("sg - pixel_format block size")
{
    using pf = sg::pixel_format;

    CHECK(sg::format_block_size(pf::undefined) == 0);
    CHECK(sg::format_block_size(pf::r8_unorm) == 1);
    CHECK(sg::format_block_size(pf::rgba8_unorm) == 4);
    CHECK(sg::format_block_size(pf::rgba32_float) == 16);
    CHECK(sg::format_block_size(pf::depth32_float_stencil8) == 8);

    // BC1 / BC4 are 8-byte blocks; the rest of BC are 16-byte blocks. Every block spans 4x4 texels.
    CHECK(sg::format_block_size(pf::bc1_rgba_unorm) == 8);
    CHECK(sg::format_block_size(pf::bc4_r_unorm) == 8);
    CHECK(sg::format_block_size(pf::bc3_unorm) == 16);
    CHECK(sg::format_block_size(pf::bc7_rgba_unorm) == 16);

    CHECK(sg::format_block_extent(pf::rgba8_unorm) == 1);
    CHECK(sg::format_block_extent(pf::bc1_rgba_unorm) == 4);
}

// --- texture_description + traits --------------------------------------------------------------------

TEST("sg - texture_description shape is derived, not flagged")
{
    // A plain 2D texture: no array, no cube, single-sampled.
    sg::texture_description d2;
    d2.format = sg::pixel_format::rgba8_unorm;
    d2.dimension = sg::texture_dimension::d2;
    d2.width = 64;
    d2.height = 32;
    CHECK(sg::traits_of(d2) == sg::texture_traits{.dimension = sg::texture_dimension::d2});

    // A single-slice 2D array is distinct from a plain 2D texture purely via array_layers being set.
    sg::texture_description d2a = d2;
    d2a.array_layers = 1;
    CHECK((sg::traits_of(d2a) == sg::texture_traits{.dimension = sg::texture_dimension::d2, .array = true}));
    CHECK(sg::traits_of(d2) != sg::traits_of(d2a));

    // Cube + multisampling fold into is_cube / sample_count, not extra dimensions.
    sg::texture_description cube;
    cube.format = sg::pixel_format::rgba8_unorm;
    cube.is_cube = true;
    CHECK((sg::traits_of(cube) == sg::texture_traits{.dimension = sg::texture_dimension::d2, .cube = true}));

    sg::texture_description ms;
    ms.format = sg::pixel_format::rgba8_unorm;
    ms.sample_count = 4;
    CHECK((sg::traits_of(ms) == sg::texture_traits{.dimension = sg::texture_dimension::d2, .multisampled = true}));
}

// The shape-gated accessors exist only where the shape has that axis — a compile-time contract.
// Detection through a template parameter keeps the constraint check SFINAE-friendly.
namespace
{
template <class T>
concept has_width = requires(T t) { t.width(); };
template <class T>
concept has_height = requires(T t) { t.height(); };
template <class T>
concept has_depth = requires(T t) { t.depth(); };
template <class T>
concept has_array_layers = requires(T t) { t.array_layers(); };
template <class T>
concept has_sample_count = requires(T t) { t.sample_count(); };
} // namespace

static_assert(has_width<sg::texture_1d>); // width is always available
static_assert(has_height<sg::texture_2d> && !has_height<sg::texture_1d>);
static_assert(has_depth<sg::texture_3d> && !has_depth<sg::texture_2d>);
static_assert(has_array_layers<sg::texture_2d_array> && !has_array_layers<sg::texture_2d>);
static_assert(has_sample_count<sg::texture_2d_ms> && !has_sample_count<sg::texture_2d>);

// The typedefs are all distinct shapes.
static_assert(sg::texture_2d::dimension == sg::texture_dimension::d2);
static_assert(!sg::texture_2d::is_array && sg::texture_2d_array::is_array);
static_assert(sg::texture_cube::is_cube && sg::texture_cube_array::is_array);
static_assert(sg::texture_2d_ms::is_multisampled);

namespace
{
// A concrete raw_texture with no GPU backing, to exercise the typed wrapper's getters + shape check.
struct test_texture final : sg::raw_texture
{
    explicit test_texture(sg::texture_description const& desc) : sg::raw_texture(desc) {}
};
} // namespace

TEST("sg - texture<Traits> wraps a matching raw_texture")
{
    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 128;
    desc.height = 64;
    desc.mip_levels = 4;

    sg::raw_texture_handle raw = std::make_shared<test_texture>(desc);
    sg::texture_2d tex(raw);

    CHECK(bool(tex));
    CHECK(tex.width() == 128);
    CHECK(tex.height() == 64);
    CHECK(tex.mip_levels() == 4);
    CHECK(tex.format() == sg::pixel_format::rgba8_unorm);
    CHECK(tex.raw() == raw); // convertible back to the raw handle
}
