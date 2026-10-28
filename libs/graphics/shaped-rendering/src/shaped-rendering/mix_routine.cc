#include <clean-core/common/assert.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/build_failure.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/mix_routine.hh>
#include <sr_sgl_shaders.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

cc::shared_async<cc::unit> mix_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shader.
    _group_layout = nullptr;
    _pipeline = {};

    auto const values = sgl_shaders::mix::options{.destination_format = params()};
    auto const pipeline = sgl_shaders::mix::main_cs.acquire_pipeline(ctx, values);
    co_await cc::async_settled(pipeline);
    if (pipeline->try_value() == nullptr)
    {
        impl::log_build_failure(pipeline, "mix", params());
        fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
        co_return;
    }

    // Both published together, after the build: readiness is what makes them visible to execute.
    _group_layout = ctx.cached.acquire_binding_group_layout(sgl_shaders::mix::mix_images::declared_bindings(values),
                                                            sgl_shaders::mix::mix_images::declared_samplers());
    _pipeline = pipeline;
    co_return;
}

bool mix_routine::execute(sg::command_list& cmd, sg::texture_2d const& destination, sg::texture_2d const& source, f32 weight)
{
    CC_ASSERT(is_set(destination), "a mix needs a destination texture");
    CC_ASSERT(is_set(source), "a mix needs a source texture");
    CC_ASSERT(destination.raw() != source.raw(), "a mix needs two different textures");
    CC_ASSERT(extent_of(destination) == extent_of(source), "a mix does not resample: the two extents differ");
    CC_ASSERT(weight >= 0.0f && weight <= 1.0f, "a mix weight must be in [0, 1]");

    // Read-only: execute touches nothing init did not publish.
    auto const self = try_acquire(cmd, destination.format());
    if (!self.is_ready())
        return false;
    auto const* const built = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (built == nullptr || *built == nullptr)
        return false;

    auto& ctx = cmd.context();
    auto const extent = extent_of(destination);

    auto const group = ctx.transient.create_binding_group(cmd, self->_group_layout,
                                                          sgl_shaders::mix::mix_images{
                                                              .source = source.as_texture_view(),
                                                              .destination = destination.as_any_image_view(),
                                                          });

    cmd.compute.bind_pipeline(**built);
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(sgl_shaders::mix::mix_constants{.weight = weight}.to_block());
    cmd.compute.dispatch_threads(extent[0], extent[1], 1);
    return true;
}
} // namespace sr
