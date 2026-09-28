#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_sgl_shaders.hh>
#include <shaped-graphics/command_list/command_list.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/resource/texture.hh>

using namespace cc::primitive_defines;

namespace shaders = sg::test::sgl_shaders;

// Every texture view dimension, and every view that binds a slice, a face or a cube as a lower dimension, read at
// one texel whose red channel names where it sits (shapes.sgl).

namespace
{
/// One layer of rgba8 texels whose red channel is `name(x, y)`, `width` × `height` of them.
template <class F>
cc::vector<u8> layer_of(int width, int height, F name)
{
    auto texels = cc::vector<u8>();
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            u8 const texel[] = {u8(name(x, y)), 0, 0, 255};
            texels.push_back_range(texel);
        }
    return texels;
}

cc::span<byte const> bytes_of(cc::vector<u8> const& texels)
{
    return cc::as_bytes(cc::span<u8 const>(texels));
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - every texture view dimension reads the slice, face or cube it names",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");

    auto const usage = sg::texture_usage::texture | sg::texture_usage::copy_dst;
    auto const format = sg::pixel_format::rgba8_unorm;

    // The names, per shape: a base, then 16 per layer, slice or face, 4 per row... wherever the shape has one.
    auto const line = ctx->persistent.create_texture_1d({.format = format, .width = 2, .usage = usage});
    auto const lines
        = ctx->persistent.create_texture_1d_array({.format = format, .width = 2, .array_layers = 3, .usage = usage});
    auto const plane = ctx->persistent.create_texture_2d({.format = format, .width = 2, .height = 2, .usage = usage});
    auto const planes = ctx->persistent.create_texture_2d_array(
        {.format = format, .width = 2, .height = 2, .array_layers = 3, .usage = usage});
    auto const volume
        = ctx->persistent.create_texture_3d({.format = format, .width = 2, .height = 2, .depth = 3, .usage = usage});
    auto const cube = ctx->persistent.create_texture_cube({.format = format, .size = 1, .usage = usage});
    auto const cubes
        = ctx->persistent.create_texture_cube_array({.format = format, .size = 1, .cube_count = 2, .usage = usage});

    auto const pipeline = co_await shaders::shapes.read_shapes.acquire_pipeline(*ctx);
    auto const layout = ctx->cached.acquire_binding_group_layout<shaders::shape_views>();
    auto const names = ctx->persistent.create_buffer_from_data(
        cc::vector<float>::create_filled(13, -1.0f), sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx->create_command_list();
    cmd->upload.bytes_to_texture(line.raw(), bytes_of(layer_of(2, 1, [](int x, int) { return 1 + x; })));
    for (auto l = 0; l < 3; ++l)
        cmd->upload.bytes_to_texture(lines.raw(), bytes_of(layer_of(2, 1, [l](int x, int) { return 10 + 4 * l + x; })),
                                     {.array_layer = l});
    cmd->upload.bytes_to_texture(plane.raw(), bytes_of(layer_of(2, 2, [](int x, int y) { return 30 + 2 * y + x; })));
    for (auto l = 0; l < 3; ++l)
        cmd->upload.bytes_to_texture(planes.raw(),
                                     bytes_of(layer_of(2, 2, [l](int x, int y) { return 40 + 4 * l + 2 * y + x; })),
                                     {.array_layer = l});
    auto volume_texels = cc::vector<u8>();
    for (auto z = 0; z < 3; ++z)
        volume_texels.push_back_range(layer_of(2, 2, [z](int x, int y) { return 60 + 4 * z + 2 * y + x; }));
    cmd->upload.bytes_to_texture(volume.raw(), bytes_of(volume_texels));
    for (auto f = 0; f < 6; ++f)
        cmd->upload.bytes_to_texture(cube.raw(), bytes_of(layer_of(1, 1, [f](int, int) { return 80 + f; })),
                                     {.array_layer = f});
    for (auto c = 0; c < 2; ++c)
        for (auto f = 0; f < 6; ++f)
            cmd->upload.bytes_to_texture(cubes.raw(),
                                         bytes_of(layer_of(1, 1, [c, f](int, int) { return 100 + 10 * c + f; })),
                                         {.array_layer = 6 * c + f});

    auto const group = ctx->transient.create_binding_group(
        *cmd, layout,
        shaders::shape_views{
            .line = line.as_texture_view(),
            .lines = lines.as_texture_view(),
            .line_of_lines = lines.as_texture_1d_view({.slice = 1}),
            .plane = plane.as_texture_view(),
            .planes = planes.as_texture_view(),
            .plane_of_planes = planes.as_texture_2d_view({.slice = 1}),
            .later_planes = planes.as_texture_view({.slices = {.start = 1, .count = 2}}),
            .volume = volume.as_texture_view(),
            .cube = cube.as_texture_view(),
            .face_of_cube = cube.as_texture_2d_view({.face = 5}),
            .faces_of_cube = cube.as_texture_2d_array_view(),
            .cubes = cubes.as_texture_view(),
            .cube_of_cubes = cubes.as_texture_cube_view({.cube = 1}),
            .names = names.as_readwrite_buffer(),
        });
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(1);
    auto const back = cmd->download.data_from_buffer(names);
    ctx->submit_command_list(cc::move(cmd));

    struct expectation
    {
        char const* what;
        int name;
    };
    constexpr expectation expected[] = {
        {.what = "1d at 1", .name = 2},
        {.what = "1d array, layer 2 at 1", .name = 19},
        {.what = "slice 1 of a 1d array as 1d", .name = 15},
        {.what = "2d at (1, 1)", .name = 33},
        {.what = "2d array, layer 2 at (1, 0)", .name = 49},
        {.what = "slice 1 of a 2d array as 2d", .name = 45},
        {.what = "a 2d array from slice 1, its layer 1 at (0, 1)", .name = 50},
        {.what = "3d at (1, 0, 2)", .name = 69},
        {.what = "cube towards +y", .name = 82},
        {.what = "face 5 of a cube as 2d", .name = 85},
        {.what = "a cube as a 2d array, layer 3", .name = 83},
        {.what = "cube array, cube 1 towards -z", .name = 115},
        {.what = "cube 1 of a cube array as a cube, towards +x", .name = 110},
    };
    auto const got = co_await back.data();
    REQUIRE(got.size() == 13);
    for (auto i = 0; i < 13; ++i)
        CHECK(int(got[i] * 255.0f + 0.5f) == expected[i].name).context(expected[i].what);
}
