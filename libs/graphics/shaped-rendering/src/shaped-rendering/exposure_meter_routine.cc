#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/exposure_meter_routine.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/scalar/scalar.hh> // tg::pow

namespace sr
{
cc::shared_async<cc::unit> exposure_meter_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shader.
    _group_layout = nullptr;
    _pipeline = {};

    auto const pipeline = sgl_shaders::exposure_meter::main_cs.acquire_pipeline(ctx);
    co_await cc::async_settled(pipeline);
    if (pipeline->try_value() == nullptr)
    {
        auto const* const why = pipeline->try_error();
        CC_LOG_ERROR("exposure_meter: the pipeline did not build{}{}", why != nullptr ? ": " : "",
                     why != nullptr ? why->underlying().to_string() : cc::string());
        fail_init();
        co_return;
    }

    _group_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::exposure_meter::meter_io>();
    _pipeline = pipeline;
    co_return;
}

cc::optional<sg::buffer<tg::vec2f>> exposure_meter_routine::execute(sg::command_list& cmd, sg::texture_2d const& image)
{
    CC_ASSERT(image.raw() != nullptr, "a meter needs an image");

    auto const self = try_acquire(cmd);
    if (!self.is_ready())
        return {};
    auto const* const built = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (built == nullptr || *built == nullptr)
        return {};

    auto& ctx = cmd.context();
    auto partials = ctx.transient.create_buffer<tg::vec2f>(
        partial_count, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);
    auto const group = ctx.transient.create_binding_group(
        cmd, self->_group_layout,
        sgl_shaders::exposure_meter::meter_io{.source = image.as_texture_view(),
                                              .partials = partials.as_readwrite_buffer()});

    cmd.compute.bind_pipeline(**built);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(partial_count, 1, 1);
    return partials;
}

cc::optional<f32> exposure_meter_routine::average_luminance(cc::span<tg::vec2f const> partials)
{
    auto sum = 0.0;
    auto count = 0.0;
    for (auto const& p : partials)
    {
        sum += f64(p[0]);
        count += f64(p[1]);
    }
    if (!(count > 0.0))
        return {};
    return f32(tg::pow(2.0, sum / count));
}
} // namespace sr
