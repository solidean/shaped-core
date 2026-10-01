#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/fsr_upscale_routine.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/fsr_backend.hh>
#include <sr_shaders.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
/// Where one of sr's own passes comes from, and the layout it binds through.
struct pass_request
{
    sg::async_compiled_shader shader;
    sg::binding_group_layout_handle group_layout;
};

/// The constants binding a compiled compute shader reflects, or null when it declares none.
[[nodiscard]] sg::binding const* constants_binding_of(sg::compiled_shader const& compiled)
{
    for (auto const& b : compiled.bindings)
        if (b.type == sg::binding_type::constants_buffer)
            return &b;
    return nullptr;
}

/// Whether `ctx` renders through WARP, whose shader compiler crashes on FSR's shading-change pyramid pass.
/// libs/graphics/shaped-rendering/docs/TODO.md records what bisecting it established.
///
/// TEMPORARY: a Microsoft-vendor adapter that DXGI does not flag as software, which a GPU-less CI runner exposes, renders
/// through WARP too, and sg's `is_software` misses it.
/// sg's adapter_info reporting such an adapter is what replaces the vendor check.
[[nodiscard]] bool renders_through_warp(sg::context const& ctx)
{
    constexpr auto k_microsoft_vendor_id = u32(0x1414);
    auto const& adapter = ctx.metrics.adapter();
    return adapter.is_software || adapter.vendor_id == k_microsoft_vendor_id;
}
} // namespace

f32 fsr_upscale_routine::ratio_of(render_scale_preset preset)
{
    // FSR's own quality modes: FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY, _BALANCED and _PERFORMANCE.
    switch (preset)
    {
    case render_scale_preset::native:
        return 1.0f;
    case render_scale_preset::quality:
        return 1.5f;
    case render_scale_preset::balanced:
        return 1.7f;
    case render_scale_preset::performance:
        return 2.0f;
    }
    return 1.0f;
}

tg::vec2f fsr_upscale_routine::jitter(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent)
{
    // FSR's offset is the one a rasterizer adds to its projection, which moves the image by it, so the sample a pixel
    // centre ends up taking lies that far the OTHER way.
    // sr's jitter is where the sample lies, hence the negation — and `fsr_dispatch` negates it back.
    return -impl::fsr_jitter_offset(frame_index, input_extent, output_extent);
}

fsr_options fsr_upscale_routine::options_for(reconstruct_settings const& settings)
{
    auto const s = settings.upscale_sharpness;
    return {.sharpening = s > 0.0f, .sharpness = s, .frame_time_ms = settings.frame_time_ms};
}

bool fsr_upscale_routine::is_available(sg::context const& ctx)
{
    if (renders_through_warp(ctx))
        return false;
    if (!impl::fsr_passes_available(ctx))
        return false;
    for (auto const& asset : {shaders::fsr_prepare_depth.compute.main_cs, shaders::fsr_clear_float.compute.main_cs,
                              shaders::fsr_clear_uint.compute.main_cs})
        if (asset == nullptr || !asset->can_acquire(ctx))
            return false;
    return true;
}

cc::shared_async<cc::unit> fsr_upscale_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shaders.
    // A stream created against the old programs holds its own reference, and recreates itself on the next call.
    _programs = nullptr;

    if (!impl::fsr_passes_available(ctx))
    {
        fail_init();
        co_return;
    }

    // AMD's passes compile while ours do.
    auto const fsr = impl::fsr_build_passes(ctx);

    pass_request requests[] = {
        {.shader = shaders::fsr_prepare_depth.compute.main_cs->acquire(ctx),
         .group_layout = ctx.cached.acquire_binding_group_layout<shaders::fsr_prepare_depth_bindings>()},
        {.shader = shaders::fsr_clear_float.compute.main_cs->acquire(ctx),
         .group_layout = ctx.cached.acquire_binding_group_layout<shaders::fsr_clear_float_bindings>()},
        {.shader = shaders::fsr_clear_uint.compute.main_cs->acquire(ctx),
         .group_layout = ctx.cached.acquire_binding_group_layout<shaders::fsr_clear_uint_bindings>()},
    };

    for (auto const& r : requests)
        co_await cc::async_settled(r.shader);

    char const* const names[] = {"fsr_prepare_depth", "fsr_clear_float", "fsr_clear_uint"};
    auto pipelines = cc::vector<sg::async_compute_pipeline>();
    for (auto i = isize(0); i < 3; ++i)
    {
        auto const& r = requests[i];
        auto const* const compiled = r.shader->try_value();
        auto const* const constants = compiled == nullptr ? nullptr : constants_binding_of(*compiled);
        if (constants == nullptr)
        {
            // A shader that did not build, or reflects no constants block to drive it by.
            auto const* const why = r.shader->try_error();
            CC_LOG_ERROR("fsr: {}.hlsl did not build: {}", names[i],
                         why != nullptr ? why->underlying().to_string() : cc::string("it reflects no constants block"));
            fail_init();
            co_return;
        }
        auto const layout
            = ctx.cached.acquire_pipeline_layout({.groups = {r.group_layout}, .inline_constants = *constants});
        pipelines.push_back(ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout}));
    }

    auto built = std::make_shared<programs>();
    programs::pass* const passes[] = {&built->prepare_depth, &built->clear_float, &built->clear_uint};
    for (auto i = isize(0); i < pipelines.size(); ++i)
    {
        co_await cc::async_settled(pipelines[i]);
        auto const* const pipeline = pipelines[i]->try_value();
        if (pipeline == nullptr)
        {
            CC_LOG_ERROR("fsr: the {} pipeline did not build", names[i]);
            fail_init();
            co_return;
        }
        passes[i]->layout = requests[i].group_layout;
        passes[i]->pipeline = *pipeline;
    }

    co_await cc::async_settled(fsr);
    auto const* const fsr_passes = fsr->try_value();
    if (fsr_passes == nullptr || *fsr_passes == nullptr)
    {
        // Building a pipeline the backend refuses throws, which arrives here as the async's error rather than a log.
        if (auto const* const why = fsr->try_error(); why != nullptr)
            CC_LOG_ERROR("fsr: its passes did not build: {}", why->underlying().to_string());
        fail_init();
        co_return;
    }
    built->fsr = *fsr_passes;

    _programs = cc::move(built);
    co_return;
}

upscale_outcome fsr_upscale_routine::execute(sg::command_list& cmd,
                                             upscale_inputs const& in,
                                             upscale_history& history,
                                             fsr_options const& options)
{
    CC_ASSERT(is_set(in.color) && is_set(in.output), "an upscale call needs a colour and an output texture");
    CC_ASSERT(is_set(in.depth) && is_set(in.motion), "FSR needs the depth and motion guides");
    CC_ASSERT(in.color.raw() != in.output.raw(), "an upscale cannot write the texture it reads");
    CC_ASSERT(extent_of(in.depth) == extent_of(in.color) && extent_of(in.motion) == extent_of(in.color),
              "the guides are at the input extent");

    auto const input_extent = extent_of(in.color);
    auto const output_extent = extent_of(in.output);
    CC_ASSERT(output_extent[0] >= input_extent[0] && output_extent[1] >= input_extent[1],
              "FSR upscales: the output must be at least the input's extent");

    if (renders_through_warp(cmd.context()) || !impl::fsr_passes_available(cmd.context()))
        return {.status = reconstruct_status::unsupported};

    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return {.status = reconstruct_status::pending};
    if (self.is_failed())
        return {.status = reconstruct_status::failed};

    auto const restarted = history._prepare(input_extent, output_extent);

    // A stream built against programs a reload has since replaced is rebuilt against the new ones.
    auto* stream = static_cast<impl::fsr_stream*>(history._state.get());
    if (stream != nullptr && impl::fsr_stream_programs(*stream) != self->_programs.get())
    {
        history._state = nullptr;
        stream = nullptr;
    }

    auto const fresh = stream == nullptr;
    if (fresh)
    {
        auto created = impl::fsr_create_stream(cmd.context(), self->_programs, input_extent, output_extent);
        if (created == nullptr)
            return {.status = reconstruct_status::failed, .restarted = restarted};
        stream = created.get();
        history._state = cc::move(created);
    }

    if (!impl::fsr_dispatch(*stream, cmd, in, options, restarted || fresh))
        return {.status = reconstruct_status::failed, .restarted = true};

    return {.status = reconstruct_status::denoised, .restarted = restarted || fresh};
}
} // namespace sr
