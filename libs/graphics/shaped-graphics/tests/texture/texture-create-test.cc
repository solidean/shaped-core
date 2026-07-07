#include <nexus/test.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/raw_texture.hh>
#include <shaped-graphics/texture.hh>

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
    desc.usage = sg::texture_usage::sampled | sg::texture_usage::copy_dst;

    auto tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex.has_value());
    CHECK(tex.value()->width() == 256);
    CHECK(tex.value()->height() == 128);
    CHECK(tex.value()->format() == sg::pixel_format::rgba8_unorm);
    CHECK(!tex.value()->is_array());
    CHECK(!tex.value()->is_multisampled());

    // The typed wrapper accepts the matching raw handle.
    sg::texture_2d typed(tex.value());
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
    desc.usage = sg::texture_usage::sampled;

    auto tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex.has_value());
    CHECK(tex.value()->is_array());
    CHECK(tex.value()->array_layers() == 1);

    sg::texture_2d_array typed(tex.value());
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
    desc.usage = sg::texture_usage::storage;

    auto tex = ctx->persistent.create_raw_texture(desc);
    REQUIRE(tex.has_value());
    CHECK(tex.value()->dimension() == sg::texture_dimension::d3);
    CHECK(tex.value()->depth() == 16);

    sg::texture_3d typed(tex.value());
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
    REQUIRE(tex.has_value());
    CHECK(tex.value()->is_valid());
    CHECK(tex.value()->width() == 128);
}
