#include <nexus/test.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-graphics/texture_descriptions.hh>

// Backend-agnostic texture creation: each is an INVOCABLE_TEST run against every available backend
// (see tests/context/context-test.cc for the mechanism). Creation only — using a texture in a command
// list (views, barriers, copies) is future work.

INVOCABLE_TEST("sg - allocates a persistent 2D texture", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 256;
    desc.height = 128;
    desc.mip_levels = 1;
    desc.usage = sg::texture_usage::readonly_texture | sg::texture_usage::copy_dst;

    auto tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex != nullptr);
    CHECK(tex->width() == 256);
    CHECK(tex->height() == 128);
    CHECK(tex->format() == sg::pixel_format::rgba8_unorm);
    CHECK(!tex->is_array());
    CHECK(!tex->is_multisampled());

    // The typed wrapper accepts the matching raw handle.
    sg::texture_2d typed(tex);
    CHECK(typed.width() == 256);
    CHECK(typed.height() == 128);
}

INVOCABLE_TEST("sg - a single-slice 2D array is distinct from a 2D texture", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 64;
    desc.height = 64;
    desc.array_layers = 1; // an array with one slice — not a plain 2D texture
    desc.usage = sg::texture_usage::readonly_texture;

    auto tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex != nullptr);
    CHECK(tex->is_array());
    CHECK(tex->array_layers() == 1);

    sg::texture_2d_array typed(tex);
    CHECK(typed.array_layers() == 1);
}

INVOCABLE_TEST("sg - allocates a persistent 3D texture", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba16_float;
    desc.dimension = sg::texture_dimension::d3;
    desc.width = 32;
    desc.height = 32;
    desc.depth = 16;
    desc.usage = sg::texture_usage::readwrite_texture;

    auto tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex != nullptr);
    CHECK(tex->dimension() == sg::texture_dimension::d3);
    CHECK(tex->depth() == 16);

    sg::texture_3d typed(tex);
    CHECK(typed.depth() == 16);
}

INVOCABLE_TEST("sg - allocates a transient texture", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_description desc;
    desc.format = sg::pixel_format::rgba8_unorm;
    desc.dimension = sg::texture_dimension::d2;
    desc.width = 128;
    desc.height = 128;
    desc.usage = sg::texture_usage::render_target;

    auto tex = ctx->transient.create_raw_texture(desc);
    REQUIRE(tex != nullptr);
    CHECK(tex->is_valid());
    CHECK(tex->width() == 128);
}

// Typed factories: a shape-specific description (only the free params) -> the wrapped texture<Traits>,
// with the shape-fixed fields filled in under the hood.

INVOCABLE_TEST("sg - typed create_texture_2d (persistent)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_2d tex = ctx->persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                                            .width = 256,
                                                            .height = 128,
                                                            .usage = sg::texture_usage::readonly_texture});

    REQUIRE(tex.raw() != nullptr);
    CHECK(tex.width() == 256);
    CHECK(tex.height() == 128);
    CHECK(tex.format() == sg::pixel_format::rgba8_unorm);
    CHECK(!tex.raw()->is_array());
    CHECK(!tex.raw()->is_multisampled());
}

INVOCABLE_TEST("sg - typed create_texture_3d (persistent)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_3d tex = ctx->persistent.create_texture_3d({.format = sg::pixel_format::rgba16_float,
                                                            .width = 32,
                                                            .height = 32,
                                                            .depth = 16,
                                                            .usage = sg::texture_usage::readwrite_texture});

    CHECK(tex.raw()->dimension() == sg::texture_dimension::d3);
    CHECK(tex.depth() == 16);
}

INVOCABLE_TEST("sg - typed create_texture_cube fills square extents (persistent)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_cube tex = ctx->persistent.create_texture_cube(
        {.format = sg::pixel_format::rgba8_unorm, .size = 64, .usage = sg::texture_usage::readonly_texture});

    CHECK(tex.raw()->is_cube());
    CHECK(tex.width() == 64);
    CHECK(tex.height() == 64); // width == height == size
}

INVOCABLE_TEST("sg - typed create_texture_2d_array (persistent)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_2d_array tex = ctx->persistent.create_texture_2d_array({.format = sg::pixel_format::rgba8_unorm,
                                                                        .width = 64,
                                                                        .height = 64,
                                                                        .array_layers = 4,
                                                                        .usage = sg::texture_usage::readonly_texture});

    CHECK(tex.raw()->is_array());
    CHECK(tex.array_layers() == 4);
}

INVOCABLE_TEST("sg - typed create_texture_2d_ms (persistent)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_2d_ms tex = ctx->persistent.create_texture_2d_ms({.format = sg::pixel_format::rgba8_unorm,
                                                                  .width = 128,
                                                                  .height = 128,
                                                                  .sample_count = 4,
                                                                  .usage = sg::texture_usage::render_target});

    CHECK(tex.raw()->is_multisampled());
    CHECK(tex.sample_count() == 4);
}

INVOCABLE_TEST("sg - typed create_texture_2d (transient, no alloc)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    sg::texture_2d tex = ctx->transient.create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 128, .height = 128, .usage = sg::texture_usage::render_target});

    REQUIRE(tex.raw() != nullptr);
    CHECK(tex.raw()->is_valid());
    CHECK(tex.width() == 128);
}

INVOCABLE_TEST("sg - typed try_create_texture_2d (persistent)", (sg::context_handle ctx))
{
    REQUIRE(ctx != nullptr);

    auto r = ctx->persistent.try_create_texture_2d(
        {.format = sg::pixel_format::rgba8_unorm, .width = 32, .height = 32, .usage = sg::texture_usage::readonly_texture});

    REQUIRE(r.has_value());
    CHECK(r.value().width() == 32);
    CHECK(r.value().height() == 32);
}
