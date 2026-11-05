#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/micromap_bake_routine.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sr
{
namespace
{
namespace file = sgl_shaders::micromap_bake;

/// How many levels a full chain over `width` x `height` has, down to 1 x 1.
int chain_length(int width, int height)
{
    auto levels = 1;
    for (auto extent = cc::max(width, height); extent > 1; extent /= 2)
        ++levels;
    return levels;
}

/// Whether `pipeline` built, logging why not.
bool has_built(sg::async_compute_pipeline const& pipeline, cc::string_view entry)
{
    if (pipeline->try_value() != nullptr)
        return true;
    auto const* const why = pipeline->try_error();
    CC_LOG_ERROR("micromap_bake: {} did not build{}{}", entry, why != nullptr ? ": " : "",
                 why != nullptr ? why->underlying().to_string() : cc::string());
    return false;
}
} // namespace

cc::shared_async<cc::unit> micromap_bake_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    _seed = {};
    _reduce = {};
    _classify = {};

    auto const seed = file::seed_cs.acquire_pipeline(ctx);
    auto const reduce = file::reduce_cs.acquire_pipeline(ctx);
    auto const classify = file::classify_cs.acquire_pipeline(ctx);
    co_await cc::async_settled(seed);
    co_await cc::async_settled(reduce);
    co_await cc::async_settled(classify);
    if (!has_built(seed, "seed_cs") || !has_built(reduce, "reduce_cs") || !has_built(classify, "classify_cs"))
    {
        fail_init();
        co_return;
    }

    _seed_layout = ctx.cached.acquire_binding_group_layout<file::pyramid_seed_io>();
    _reduce_layout = ctx.cached.acquire_binding_group_layout<file::pyramid_reduce_io>();
    _classify_layout = ctx.cached.acquire_binding_group_layout<file::classify_io>();
    _seed = seed;
    _reduce = reduce;
    _classify = classify;
    co_return;
}

int micromap_bake_routine::level_for(tg::vec2f a, tg::vec2f b, tg::vec2f c, int width, int height, int max_level)
{
    auto const e1x = (b[0] - a[0]) * f32(width);
    auto const e1y = (b[1] - a[1]) * f32(height);
    auto const e2x = (c[0] - a[0]) * f32(width);
    auto const e2y = (c[1] - a[1]) * f32(height);
    auto const texels = tg::abs(e1x * e2y - e1y * e2x) * 0.5f;
    // 4^level micro-triangles, the first count that reaches the area
    auto level = 0;
    while (level < max_level && f32(1 << (2 * level)) < texels)
        ++level;
    return level;
}

cc::optional<micromap_bake> micromap_bake_routine::execute(sg::command_list& cmd, micromap_bake_input const& input)
{
    CC_ASSERT(input.opacity.raw() != nullptr, "a bake needs an opacity texture");
    CC_ASSERT(!input.indices.empty() && input.indices.size() % 3 == 0, "a bake needs whole triangles");
    CC_ASSERT(input.channel >= 0 && input.channel < 4, "the opacity channel is a component, 0 to 3");
    CC_ASSERT(input.max_level >= 0 && input.max_level <= 12, "both APIs subdivide a 4-state map at most 12 times");
    CC_ASSERT(cmd.context().supports(sg::feature::opacity_micromap),
              "micromap_bake: this context has no sg::feature::opacity_micromap (check ctx.supports first)");

    auto const self = try_acquire(cmd);
    if (!self.is_ready() || self->_classify == nullptr)
        return {};
    auto& ctx = cmd.context();
    auto const width = int(input.opacity.width());
    auto const height = int(input.opacity.height());
    auto const levels = chain_length(width, height);

    // ---- the layout, which both APIs size the build from before it runs
    auto const triangle_count = input.indices.size() / 3;
    auto maps = cc::vector<tg::vec4i>();
    auto word_maps = cc::vector<i32>();
    auto descriptors = cc::vector<sg::micromap_descriptor>();
    auto map_indices = cc::vector<u32>();
    auto usage = cc::vector<sg::micromap_usage>();
    for (auto t = isize(0); t < triangle_count; ++t)
    {
        auto const& i = input.indices;
        auto const level = level_for(input.uvs[i[t * 3]], input.uvs[i[t * 3 + 1]], input.uvs[i[t * 3 + 2]], width,
                                     height, input.max_level);
        auto const first_word = i32(word_maps.size());
        auto const words = ((1 << (2 * level)) + 15) / 16;
        for (auto w = 0; w < words; ++w)
            word_maps.push_back(i32(t));
        maps.push_back(tg::vec4i(i32(t), level, first_word, 0));
        descriptors.push_back({.states_offset_in_bytes = u32(first_word) * 4,
                               .subdivision_level = u16(level),
                               .format = sg::micromap_format::four_state});
        map_indices.push_back(u32(t));
        auto* counted = static_cast<sg::micromap_usage*>(nullptr);
        for (auto& u : usage)
            if (u.subdivision_level == u32(level))
                counted = &u;
        if (counted == nullptr)
        {
            usage.push_back({.count = 0, .subdivision_level = u32(level)});
            counted = &usage.back();
        }
        ++counted->count;
    }
    auto uv_indices = cc::vector<i32>();
    for (auto const index : input.indices)
        uv_indices.push_back(i32(index));

    auto const build_input = sg::buffer_usage::accel_structure_build_input;
    auto const states
        = ctx.transient.create_buffer<u32>(word_maps.size(), sg::buffer_usage::readwrite_buffer | build_input);
    auto const readonly = sg::buffer_usage::readonly_buffer;
    auto const descriptor_buffer = ctx.transient.create_buffer_from_data(cmd, descriptors, build_input);
    auto const index_buffer = ctx.transient.create_buffer_from_data(cmd, map_indices, build_input);
    auto const uv_buffer = ctx.transient.create_buffer_from_data(cmd, input.uvs, readonly);
    auto const uv_index_buffer = ctx.transient.create_buffer_from_data(cmd, uv_indices, readonly);
    auto const map_buffer = ctx.transient.create_buffer_from_data(cmd, maps, readonly);
    auto const word_map_buffer = ctx.transient.create_buffer_from_data(cmd, word_maps, readonly);

    // ---- the min/max pyramid of the channel
    auto const pyramid
        = ctx.transient.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                           .width = width,
                                           .height = height,
                                           .mip_levels = levels,
                                           .usage = sg::texture_usage::texture | sg::texture_usage::image});
    {
        auto const group = ctx.transient.create_binding_group(
            cmd, self->_seed_layout,
            file::pyramid_seed_io{.source = input.opacity.as_texture_view(),
                                  .target = pyramid.as_image_view<sg::pixel_format::rgba32_float>({.mip = 0})});
        cmd.compute.bind_pipeline(**self->_seed->try_value());
        cmd.compute.bind_group(0, *group);
        cmd.compute.set_inline_constants(
            file::pyramid_constants{.channel = input.channel, .scale = input.scale, .bias = input.bias});
        cmd.compute.dispatch_threads(width, height, 1);
    }
    for (auto level = 1; level < levels; ++level)
    {
        auto const group = ctx.transient.create_binding_group(
            cmd, self->_reduce_layout,
            file::pyramid_reduce_io{.source = pyramid.as_texture_view({.mips = {.start = level - 1, .count = 1}}),
                                    .target = pyramid.as_image_view<sg::pixel_format::rgba32_float>({.mip = level})});
        cmd.compute.bind_pipeline(**self->_reduce->try_value());
        cmd.compute.bind_group(0, *group);
        cmd.compute.dispatch_threads(cc::max(width >> level, 1), cc::max(height >> level, 1), 1);
    }

    // ---- the states, a word of 16 micro-triangles a thread
    {
        auto const group = ctx.transient.create_binding_group(cmd, self->_classify_layout,
                                                              file::classify_io{
                                                                  .pyramid = pyramid.as_texture_view(),
                                                                  .uvs = uv_buffer.as_readonly_buffer(),
                                                                  .indices = uv_index_buffer.as_readonly_buffer(),
                                                                  .maps = map_buffer.as_readonly_buffer(),
                                                                  .word_maps = word_map_buffer.as_readonly_buffer(),
                                                                  .states = states.as_readwrite_buffer(),
                                                              });
        cmd.compute.bind_pipeline(**self->_classify->try_value());
        cmd.compute.bind_group(0, *group);
        cmd.compute.set_inline_constants(file::classify_constants{.word_count = i32(word_maps.size()),
                                                                  .cutoff = input.cutoff,
                                                                  .levels = levels,
                                                                  .clamps = input.clamps ? 1 : 0});
        cmd.compute.dispatch_threads(i32(word_maps.size()), 1, 1);
    }

    return micromap_bake{
        .triangles = {.states = states.raw(), .descriptors = descriptor_buffer.raw(), .usage = cc::move(usage)},
        .indices = index_buffer.raw()};
}
} // namespace sr
