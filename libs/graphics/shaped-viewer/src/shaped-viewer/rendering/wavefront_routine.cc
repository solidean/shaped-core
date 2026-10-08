#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/shader_library.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
namespace tracer = sv::shaders::tracer;

/// `dispatch.args`' records, as module `tracer` names them (`dispatch_*` in shaders/sgl/tracer_wavefront.sgl).
constexpr isize dispatch_round = 0;
constexpr isize dispatch_hits = 1;
constexpr isize dispatch_shadow = 2;
constexpr isize dispatch_finished = 3;
constexpr isize dispatch_shade = 4;

/// `wavefront_step::fill` that starts every free slot (`fill_full` in shaders/sgl/tracer_wavefront.sgl).
constexpr u32 fill_full = 65536;

/// `wavefront_step::mode` of ReSTIR's re-shade (`step_restir_shade` in shaders/sgl/tracer_wavefront.sgl).
constexpr u32 restir_shade_mode = 1;

/// `frame_constants::wavefront_flags`, as `wavefront_flag_*` in shaders/sgl/tracer_wavefront.sgl name them.
constexpr u32 wavefront_flag_restart = 1;
constexpr u32 wavefront_flag_restir = 2;

/// The context's first shader format the library builds SGL into, as a compile of a package's asset would pick.
[[nodiscard]] cc::optional<sg::shader_format> sgl_format_for(sg::context const& ctx, slib::shader_library const& lib)
{
    for (auto const f : ctx.accepted_shader_formats())
        if (lib.can_compile(slib::shader_language::sgl, f))
            return f;
    return {};
}
} // namespace

cc::shared_async<cc::unit> wavefront_routine::init_once(sg::routine_init_scope scope)
{
    _stand_ins = impl::traced_stand_ins::create(scope.context());
    co_return;
}

cc::shared_async<cc::unit> wavefront_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    auto const& w = sv::shaders::wavefront;
    _kernels = kernels{
        .begin = co_await w.begin.acquire_pipeline(ctx),
        .extend = co_await w.extend.acquire_pipeline(ctx),
        .segment = co_await w.segment.acquire_pipeline(ctx),
        .sort = co_await w.sort.acquire_pipeline(ctx),
        .connect = co_await w.connect.acquire_pipeline(ctx),
        .finalize = co_await w.finalize.acquire_pipeline(ctx),
        .resolve = co_await w.resolve.acquire_pipeline(ctx),
        .reset = co_await w.reset.acquire_pipeline(ctx),
        .restir_gather = co_await w.restir_gather.acquire_pipeline(ctx),
        .args_round = co_await w.args_round.acquire_pipeline(ctx),
        .args_shade = co_await w.args_shade.acquire_pipeline(ctx),
        .args_connect = co_await w.args_connect.acquire_pipeline(ctx),
    };
    // A reload re-generates every shade kernel against modules that may have moved.
    _shade_kernels.clear();
}

sg::compute_pipeline_handle wavefront_routine::_shade_kernel_of(sg::context& ctx,
                                                                material_permutation const* p,
                                                                bound_resources const& bindless)
{
    auto& k = _shade_kernels.entry(p->key).get_or_emplace(shade_kernel{});
    if (k.pipeline != nullptr)
        return k.pipeline;
    if (k.failed)
        return nullptr;

    if (!k.shader.is_valid())
    {
        auto lib = acquire_shader_library();
        auto const format = lib.has_value() ? sgl_format_for(ctx, *lib.value()) : cc::optional<sg::shader_format>();
        if (!format.has_value())
        {
            CC_LOG_ERROR("no shader library builds {}'s shade kernel into a format this context accepts", p->label);
            k.failed = true;
            return nullptr;
        }
        k.shader = lib.value()->compile_source(p->shade_source, sg::shader_stage::compute, "sv_shade", format.value(),
                                               {.language = slib::shader_language::sgl, .label = p->label});
        (void)ctx.backlog.start(k.shader);
        return nullptr;
    }

    if (!k.pending.is_valid())
    {
        if (!k.shader->is_ready())
            return nullptr;
        auto const* const compiled = k.shader->try_value();
        if (compiled == nullptr)
        {
            CC_LOG_ERROR("the shade kernel of {} did not compile, so its geometry shades as the stand-in: {}", p->label,
                         k.shader->try_error()->underlying().to_string());
            k.failed = true;
            return nullptr;
        }
        // Group 1 is the manager's own layout, since a group fits only the very layout it was created against.
        auto groups = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
        groups.push_back(ctx.cached.acquire_binding_group_layout<tracer::traced>());
        groups.push_back(bindless.layout());
        groups.push_back(ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>());
        auto const layout = ctx.cached.acquire_pipeline_layout(
            {.groups = cc::move(groups), .inline_constants = tracer::wavefront_step::inline_binding()});
        k.pending = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = layout});
        (void)ctx.backlog.start(k.pending);
        return nullptr;
    }

    if (!k.pending->is_ready())
        return nullptr;
    auto const* const built = k.pending->try_value();
    if (built == nullptr)
    {
        CC_LOG_ERROR("the shade kernel of {} did not build into a pipeline", p->label);
        k.failed = true;
        return nullptr;
    }
    k.pipeline = *built;
    return k.pipeline;
}

cc::optional<cc::fixed_array<f64, wavefront_timing::stage_count>> wavefront_timing::try_seconds() const
{
    if (starts.empty())
        return {};
    auto const last = end.try_get_seconds();
    if (!last.has_value())
        return {};

    auto seconds = cc::fixed_array<f64, stage_count>{};
    for (auto i = isize(0); i < starts.size(); ++i)
    {
        auto const from = starts[i].try_get_seconds();
        auto const to = i + 1 < starts.size() ? starts[i + 1].try_get_seconds() : last;
        if (!from.has_value() || !to.has_value())
            return {};
        seconds[isize(stages[i])] += to.value() - from.value();
    }
    return seconds;
}

void wavefront_routine::_record_landed_timings()
{
    // A frame whose list was dropped never lands; a handful in flight is the most a healthy queue holds.
    constexpr isize max_in_flight = 8;
    while (_timed.size() > max_in_flight)
        _timed.remove_at(0);

    while (!_timed.empty())
    {
        auto const landed = _timed.front().try_seconds();
        if (!landed.has_value())
            return;
        auto const& seconds = landed.value();
        auto const at = [&](wavefront_stage s) { return seconds[isize(s)]; };
        CC_RECORD_STAT("sv.wavefront.begin", cc::rec::unit_seconds, at(wavefront_stage::begin));
        CC_RECORD_STAT("sv.wavefront.extend", cc::rec::unit_seconds, at(wavefront_stage::extend));
        CC_RECORD_STAT("sv.wavefront.segment", cc::rec::unit_seconds, at(wavefront_stage::segment));
        CC_RECORD_STAT("sv.wavefront.sort", cc::rec::unit_seconds, at(wavefront_stage::sort));
        CC_RECORD_STAT("sv.wavefront.shade", cc::rec::unit_seconds, at(wavefront_stage::shade));
        CC_RECORD_STAT("sv.wavefront.connect", cc::rec::unit_seconds, at(wavefront_stage::connect));
        CC_RECORD_STAT("sv.wavefront.finalize", cc::rec::unit_seconds, at(wavefront_stage::finalize));
        CC_RECORD_STAT("sv.wavefront.arguments", cc::rec::unit_seconds, at(wavefront_stage::arguments));
        // Only a frame that ran the re-shade has a time for it; the others would record a 0 that reads as a measurement.
        if (at(wavefront_stage::restir) > 0.0)
            CC_RECORD_STAT("sv.wavefront.restir", cc::rec::unit_seconds, at(wavefront_stage::restir));
        CC_RECORD_STAT("sv.wavefront.resolve", cc::rec::unit_seconds, at(wavefront_stage::resolve));
        _timed.remove_at(0);
    }
}

sg::routine_outcome wavefront_routine::execute(sg::command_list& cmd, wavefront_trace_desc const& d)
{
    CC_RECORD_SCOPE("sv.wavefront");

    // Exclusive: _shade_kernel_of writes the kernel map.
    auto self = try_acquire_exclusive(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();
    auto const& t = d.trace;

    CC_ASSERT(d.state != nullptr, "wavefront_routine: a trace continues a view's state");
    CC_ASSERT(t.output.raw() != nullptr, "wavefront_routine: no output target bound");
    CC_ASSERT(t.output.raw()->description().format == sg::pixel_format::rgba32_float,
              "wavefront_routine: the target is rgba32_float, which the mean is written into");
    CC_ASSERT(d.state->extent == tg::vec2i(t.output.width(), t.output.height()),
              "wavefront_routine: the state's extent must be the output's — a state belongs to one view at one size");
    CC_ASSERT(!t.hit_groups.empty(), "wavefront_routine: a trace needs at least one permutation to shade with");
    CC_ASSERT(isize(t.hit_groups.size()) <= isize(d.state->max_permutations),
              "wavefront_routine: the trace names more permutations than its state has room for");
    CC_ASSERT(t.bindless != nullptr, "wavefront_routine: a trace binds the manager's bindless tables");
    CC_ASSERT(d.rounds > 0, "wavefront_routine: a frame takes at least one round");
    CC_ASSERT(!d.restir || (d.restart && !d.regenerate), "wavefront_routine: only a draining frame keeps ReSTIR's "
                                                         "records");

    // Started whether or not a substitution ends up needing them.
    auto const fallback = t.fallback != nullptr ? self->_shade_kernel_of(ctx, t.fallback, *t.bindless) : nullptr;
    auto const quadric_fallback
        = t.quadric_fallback != nullptr ? self->_shade_kernel_of(ctx, t.quadric_fallback, *t.bindless) : nullptr;

    auto shade = cc::vector<sg::compute_pipeline_handle>();
    shade.reserve(t.hit_groups.size());
    auto substituted = false;
    for (auto const* p : t.hit_groups)
    {
        CC_ASSERT(p != nullptr, "a wavefront trace names a permutation the shader cache does not hold");
        auto kernel = self->_shade_kernel_of(ctx, p, *t.bindless);
        if (kernel == nullptr)
        {
            kernel = p->kind == geometry_kind::quadrics ? quadric_fallback : fallback;
            substituted = true;
        }
        if (kernel == nullptr)
            return sg::routine_outcome::declined;
        shade.push_back(cc::move(kernel));
    }

    auto const tlas = cmd.raytracing.build_tlas(t.instances);

    // A binding cannot be empty, so a trace with no lights binds one zeroed record that `light_count == 0` never reads.
    auto lights = t.lights;
    if (lights.raw() == nullptr)
    {
        lights = ctx.transient.create_buffer<tracer::light_record>(
            1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(lights, tracer::light_record{});
    }

    auto fc = d.frame;
    fc.generation = d.generation;
    if (d.restir)
        d.state->ensure_restir(ctx);
    fc.wavefront_flags = (d.restart ? wavefront_flag_restart : 0u) | (d.restir ? wavefront_flag_restir : 0u);
    fc.permutation_count = u32(t.hit_groups.size());
    fc.roulette_after = d.roulette_after;
    auto const frame = ctx.transient.create_buffer_from_pod(cmd, fc, sg::buffer_usage::readonly_buffer);

    auto const traced
        = ctx.transient.create_binding_group(cmd, ctx.cached.acquire_binding_group_layout<tracer::traced>(),
                                             impl::traced_group_of(t, self->_stand_ins, tlas, lights, frame, *d.state));
    auto const tables = impl::decal_tables_of(cmd, t, self->_stand_ins);
    auto const counting
        = ctx.transient.create_binding_group(cmd, ctx.cached.acquire_binding_group_layout<tracer::dispatch>(),
                                             tracer::dispatch{.args = d.state->args.as_readwrite_buffer()});
    auto const args = d.state->args.reinterpret_as<sg::dispatch_args>();

    auto const width = u32(d.state->extent[0]);
    auto const height = u32(d.state->extent[1]);
    auto step = tracer::wavefront_step{.parity = 0,
                                       .permutation = 0,
                                       .regenerate = d.regenerate ? 1u : 0u,
                                       .width = width,
                                       .height = height,
                                       .fill = u32(cc::clamp(d.fill, 0.0f, 1.0f) * f32(fill_full) + 0.5f),
                                       .mode = 0,
                                       .pad0 = 0};

    // What a kernel's groups are: `tracer.traced` at 0 always, then the bindless tables or the arguments at 1, and the decal tables at 2.
    enum class second_group
    {
        none,
        bindless,
        counting,
    };
    // Every table stays declared for each dispatch that binds them, until `declared` dies at the end of execute.
    // The first extend applies them all, and every later bindless dispatch finds them current and touches no element.
    // The kernels that bind no table leave them dormant.
    auto const declared = t.bindless->declare_compute_access(cmd);

    auto const use = [&](sg::compute_pipeline const& kernel, second_group second)
    {
        cmd.compute.bind_pipeline(kernel);
        cmd.compute.bind_group(0, *traced);
        if (second == second_group::bindless)
            cmd.compute.bind_group(bindless_group, *t.bindless->group());
        else if (second == second_group::counting)
            cmd.compute.bind_group(1, *counting);
        cmd.compute.set_inline_constants(step);
    };

    self->_record_landed_timings();
    auto timing = wavefront_timing{};
    auto const timed = cmd.query.is_supported();
    auto const mark = [&](wavefront_stage s)
    {
        if (!timed)
            return;
        timing.stages.push_back(s);
        timing.starts.push_back(cmd.query.record_gpu_timestamp());
    };

    // One counter per round, copied out of `counters` before the round's own kernels move it.
    auto live = sg::buffer<u32>();
    if (d.live_paths != nullptr)
        live = ctx.transient.create_buffer<u32>(d.rounds, sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src);

    mark(wavefront_stage::begin);
    use(*self->_kernels.reset, second_group::none);
    cmd.compute.dispatch_groups(1);
    use(*self->_kernels.begin, second_group::none);
    cmd.compute.dispatch_threads(int(width * height));

    for (auto round = 0; round < d.rounds; ++round)
    {
        step.parity = u32(round % 2);
        step.permutation = 0;

        if (live.raw() != nullptr)
            cmd.copy.buffer_data_region<u32>({.src = d.state->counters.raw(),
                                              .dst = live.raw(),
                                              .count = 1,
                                              .src_offset = round % 2,
                                              .dst_offset = round});

        mark(wavefront_stage::arguments);
        use(*self->_kernels.args_round, second_group::counting);
        cmd.compute.dispatch_groups(1);
        mark(wavefront_stage::extend);
        use(*self->_kernels.extend, second_group::bindless);
        cmd.compute.dispatch_groups_indirect(args, dispatch_round);
        mark(wavefront_stage::segment);
        use(*self->_kernels.segment, second_group::none);
        cmd.compute.dispatch_groups_indirect(args, dispatch_round);

        mark(wavefront_stage::arguments);
        use(*self->_kernels.args_shade, second_group::counting);
        cmd.compute.dispatch_groups(1);
        mark(wavefront_stage::sort);
        use(*self->_kernels.sort, second_group::none);
        cmd.compute.dispatch_groups_indirect(args, dispatch_hits);

        mark(wavefront_stage::shade);
        for (auto p = isize(0); p < shade.size(); ++p)
        {
            step.permutation = u32(p);
            use(*shade[p], second_group::bindless);
            cmd.compute.bind_group(2, *tables);
            cmd.compute.dispatch_groups_indirect(args, dispatch_shade + p);
        }
        step.permutation = 0;

        mark(wavefront_stage::arguments);
        use(*self->_kernels.args_connect, second_group::counting);
        cmd.compute.dispatch_groups(1);
        mark(wavefront_stage::connect);
        use(*self->_kernels.connect, second_group::bindless);
        cmd.compute.dispatch_groups_indirect(args, dispatch_shadow);
        mark(wavefront_stage::finalize);
        use(*self->_kernels.finalize, second_group::none);
        cmd.compute.dispatch_groups_indirect(args, dispatch_finished);
    }

    if (d.restir)
    {
        // The persisted primary hits, through the round's own sort and shade kernels.
        // Counters cleared, the hits gathered by permutation and sorted, and each permutation's kernel run in its re-shade mode.
        mark(wavefront_stage::restir);
        step.parity = 0;
        step.permutation = 0;
        step.mode = restir_shade_mode;
        use(*self->_kernels.reset, second_group::none);
        cmd.compute.dispatch_groups(1);
        use(*self->_kernels.restir_gather, second_group::none);
        cmd.compute.dispatch_threads(int(width * height));
        use(*self->_kernels.args_shade, second_group::counting);
        cmd.compute.dispatch_groups(1);
        use(*self->_kernels.sort, second_group::none);
        cmd.compute.dispatch_groups_indirect(args, dispatch_hits);
        for (auto p = isize(0); p < shade.size(); ++p)
        {
            step.permutation = u32(p);
            use(*shade[p], second_group::bindless);
            cmd.compute.bind_group(2, *tables);
            cmd.compute.dispatch_groups_indirect(args, dispatch_shade + p);
        }
        step.permutation = 0;
        step.mode = 0;
    }

    mark(wavefront_stage::resolve);
    use(*self->_kernels.resolve, second_group::none);
    cmd.compute.dispatch_threads(int(width * height));

    if (timed)
    {
        timing.end = cmd.query.record_gpu_timestamp();
        if (d.timing != nullptr)
            *d.timing = timing;
        self->_timed.push_back(cc::move(timing));
    }
    if (live.raw() != nullptr)
        *d.live_paths = cmd.download.data_from_buffer(live);
    if (t.traced_with_stand_ins != nullptr)
        *t.traced_with_stand_ins = substituted;
    return sg::routine_outcome::executed;
}
} // namespace sv
