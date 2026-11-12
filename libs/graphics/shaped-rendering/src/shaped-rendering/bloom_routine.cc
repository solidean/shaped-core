#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/bloom_routine.hh>
#include <sr_sgl_shaders.hh>

namespace sr
{
namespace
{
namespace shader = sgl_shaders::bloom;

/// One Gaussian of the kernel: its radius as a share of the image's height, and its weight.
struct gaussian
{
    f64 radius = 0;
    f64 weight = 0;
};
constexpr gaussian k_kernel[] = {{0.01, 0.4}, {0.03, 0.3}, {0.08, 0.2}, {0.2, 0.1}};

/// How many levels it takes to reach the widest Gaussian, without halving the image's smaller side away.
[[nodiscard]] int level_count_for(f64 widest_sigma, int smallest_side)
{
    auto count = 1;
    while (bloom_routine::level_sigma(count - 1) < widest_sigma && (smallest_side >> (count + 1)) >= 2)
        ++count;
    return count;
}

[[nodiscard]] bool is_built(sg::async_compute_pipeline const& p)
{
    if (p == nullptr)
        return false;
    auto const* const value = p->try_value();
    return value != nullptr && *value != nullptr;
}
} // namespace

f64 bloom_routine::level_sigma(int index)
{
    return 2.16 * f64(1 << index);
}

cc::vector<f32> bloom_routine::level_weights(int height, f32 radius_scale)
{
    auto const scale = f64(radius_scale) * height;
    auto const count = level_count_for(k_kernel[3].radius * scale, height);
    auto weights = cc::vector<f32>();
    for (auto i = 0; i < count; ++i)
        weights.push_back(0.0f);

    for (auto const& g : k_kernel)
    {
        auto const sigma = g.radius * scale;
        auto m = 0;
        while (m < count - 1 && level_sigma(m + 1) < sigma)
            ++m;
        if (m == count - 1 || sigma <= level_sigma(0))
        {
            weights[m] += f32(g.weight);
            continue;
        }
        auto const lo = level_sigma(m) * level_sigma(m);
        auto const hi = level_sigma(m + 1) * level_sigma(m + 1);
        auto const t = (sigma * sigma - lo) / (hi - lo);
        weights[m] += f32(g.weight * (1.0 - t));
        weights[m + 1] += f32(g.weight * t);
    }
    return weights;
}

cc::shared_async<cc::unit> bloom_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shader.
    _first = {};
    _down = {};
    _up = {};
    _finish = {};

    auto const first = shader::first_cs.acquire_pipeline(ctx);
    auto const down = shader::down_cs.acquire_pipeline(ctx);
    auto const up = shader::up_cs.acquire_pipeline(ctx);
    auto const finish = shader::finish_cs.acquire_pipeline(ctx);
    for (auto const* const p : {&first, &down, &up, &finish})
    {
        co_await cc::async_settled(*p);
        if ((*p)->try_value() == nullptr)
        {
            auto const* const why = (*p)->try_error();
            CC_LOG_ERROR("bloom: a pipeline did not build{}{}", why != nullptr ? ": " : "",
                         why != nullptr ? why->underlying().to_string() : cc::string());
            fail_init();
            co_return;
        }
    }

    _first_layout = ctx.cached.acquire_binding_group_layout<shader::bloom_first_io>();
    _chain_layout = ctx.cached.acquire_binding_group_layout<shader::bloom_chain_io>();
    _finish_layout = ctx.cached.acquire_binding_group_layout<shader::bloom_finish_io>();
    _first = first;
    _down = down;
    _up = up;
    _finish = finish;
    co_return;
}

cc::optional<sg::texture_2d> bloom_routine::execute(sg::command_list& cmd,
                                                    sg::texture_2d const& image,
                                                    bloom_settings const& settings,
                                                    f32 exposure_scale)
{
    CC_ASSERT(image.raw() != nullptr, "bloom needs an image");
    CC_ASSERT(exposure_scale > 0.0f, "an exposure scale is positive");

    auto const self = try_acquire(cmd);
    if (!self.is_ready() || !is_built(self->_first) || !is_built(self->_down) || !is_built(self->_up)
        || !is_built(self->_finish))
        return {};

    auto& ctx = cmd.context();
    auto const width = image.width();
    auto const height = image.height();
    auto const weights = level_weights(height, settings.radius_scale);
    auto const levels = i32(weights.size());
    auto const knee = settings.knee / exposure_scale;

    auto const create = [&](int w, int h)
    {
        return ctx.transient.create_texture_2d(
            {.format = sg::pixel_format::rgba16_float,
             .width = cc::max(w, 1),
             .height = cc::max(h, 1),
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_src});
    };

    auto chain = cc::vector<sg::texture_2d>();
    for (auto i = 0; i < levels; ++i)
        chain.push_back(create(width >> (i + 1), height >> (i + 1)));

    auto const dispatch = [&](sg::async_compute_pipeline const& pipeline, auto const& group,
                              sg::texture_2d const& target, f32 weight, f32 carry)
    {
        cmd.compute.bind_pipeline(**pipeline->try_value());
        cmd.compute.bind_group(0, *group);
        if (pipeline != self->_down) // halving reads no constants, so its layout has no block for them
            cmd.compute.set_inline_constants(
                shader::bloom_constants{.knee = knee, .weight = weight, .carry = carry}.to_block());
        cmd.compute.dispatch_threads(target.width(), target.height(), 1);
    };

    auto const first_group = ctx.transient.create_binding_group(
        cmd, self->_first_layout,
        shader::bloom_first_io{.source = image.as_texture_view(),
                               .target = chain[0].as_image_view<sg::pixel_format::rgba16_float>()});
    dispatch(self->_first, first_group, chain[0], 0.0f, 0.0f);

    for (auto i = 1; i < levels; ++i)
    {
        auto const group
            = ctx.transient.create_binding_group(cmd, self->_chain_layout,
                                                 shader::bloom_chain_io{
                                                     .source = chain[i - 1].as_texture_view(),
                                                     .level = chain[i - 1].as_texture_view(),
                                                     .target = chain[i].as_image_view<sg::pixel_format::rgba16_float>(),
                                                 });
        dispatch(self->_down, group, chain[i], 0.0f, 0.0f);
    }

    // The coarsest level has not been weighted yet, and what comes up from any other has.
    auto carried = chain[levels - 1];
    auto carry = weights[levels - 1];
    for (auto i = levels - 2; i >= 0; --i)
    {
        auto target = create(chain[i].width(), chain[i].height());
        auto const group
            = ctx.transient.create_binding_group(cmd, self->_chain_layout,
                                                 shader::bloom_chain_io{
                                                     .source = carried.as_texture_view(),
                                                     .level = chain[i].as_texture_view(),
                                                     .target = target.as_image_view<sg::pixel_format::rgba16_float>(),
                                                 });
        dispatch(self->_up, group, target, weights[i], carry);
        carried = target;
        carry = 1.0f;
    }

    auto out = create(width, height);
    auto const finish_group
        = ctx.transient.create_binding_group(cmd, self->_finish_layout,
                                             shader::bloom_finish_io{
                                                 .source = image.as_texture_view(),
                                                 .spread = carried.as_texture_view(),
                                                 .target = out.as_image_view<sg::pixel_format::rgba16_float>(),
                                             });
    dispatch(self->_finish, finish_group, out, settings.strength, carry);
    return out;
}
} // namespace sr
