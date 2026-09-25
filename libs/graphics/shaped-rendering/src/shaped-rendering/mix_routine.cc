#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh> // offsetof
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/mix_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
/// The inline-constants block mix.hlsl declares, byte for byte.
struct mix_constants_gpu
{
    f32 weight = 0.0f;
    f32 _pad[3] = {};
};

static_assert(sizeof(mix_constants_gpu) == sizeof(sr::shaders::mix_constants),
              "the mix constants are not the size mix.hlsl's block states");
static_assert(offsetof(sr::shaders::mix_constants, weight) == offsetof(mix_constants_gpu, weight),
              "weight moved in mix.hlsl");
} // namespace

cc::shared_async<cc::unit> mix_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shader.
    _group_layout = nullptr;
    _pipeline = nullptr;

    auto const shader = sr::shaders::mix.compute.main_cs->acquire(ctx);
    co_await cc::async_settled(shader);
    auto const* const compiled = shader->try_value();
    if (compiled == nullptr)
    {
        fail_init();
        co_return;
    }

    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    if (constants_binding == nullptr)
    {
        fail_init(); // a shader that reflects no constants block cannot be driven
        co_return;
    }

    auto group_layout = ctx.cached.acquire_binding_group_layout<shaders::mix_bindings>();
    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}, .inline_constants = *constants_binding});

    auto const pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    if (built == nullptr)
    {
        fail_init();
        co_return;
    }

    // Both published together, after the build: readiness is what makes them visible to execute.
    _group_layout = cc::move(group_layout);
    _pipeline = *built;
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
    auto const self = try_acquire(cmd);
    if (!self.is_ready())
        return false;

    auto& ctx = cmd.context();
    auto const extent = extent_of(destination);

    auto const group = ctx.transient.create_binding_group(cmd, self->_group_layout,
                                                          shaders::mix_bindings{
                                                              .gSource = source.as_readonly_view(),
                                                              .gDestination = destination.as_readwrite_view(),
                                                          });

    cmd.compute.bind_pipeline(*self->_pipeline);
    cmd.compute.bind<shaders::mix_bindings>(*group);
    cmd.compute.set_inline_constants(mix_constants_gpu{.weight = weight});
    cmd.compute.dispatch_threads(extent[0], extent[1], 1);
    return true;
}
} // namespace sr
