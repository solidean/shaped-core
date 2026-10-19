#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/compute/compute_pipeline.hh>
#include <shaped-graphics/query/gpu_timestamp.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/traced_group.hh>

/// What a wavefront frame's timestamps divide its work into; each stage is summed over the frame's rounds.
/// `arguments` is the single-thread kernels that turn a queue's count into the next dispatch's size.
enum class sv::wavefront_stage : sv::u8
{
    begin,
    extend,
    segment,
    sort,
    shade,
    connect,
    finalize,
    arguments,
    resolve,
};

/// One wavefront frame's GPU timestamps, each marking where a stage starts; `end` closes the last.
/// Empty where the device does not time its queue, and readable only once the list has run.
struct sv::wavefront_timing
{
    /// How many `wavefront_stage` values there are.
    static constexpr isize stage_count = 9;

    cc::vector<wavefront_stage> stages;
    cc::vector<sg::gpu_timestamp> starts;
    sg::gpu_timestamp end;

    /// Each stage's GPU seconds, indexed by `wavefront_stage`, or nullopt while a timestamp has not landed or none was recorded.
    [[nodiscard]] cc::optional<cc::fixed_array<f64, stage_count>> try_seconds() const;
};

/// One frame of the wavefront tracer: what to trace, the state it continues, and how far it may go.
struct sv::wavefront_trace_desc
{
    /// Everything the scene binds, as the megakernel takes it.
    /// `trace.frame` is not read: the routine uploads `frame` itself, with the wavefront's fields filled from below.
    pt_trace_desc const& trace;

    /// The frame block: the camera, the sample controls, the lights' table.
    /// `samples_per_pixel` is not read — a frame's samples are what its rounds finish — and `accum_frame` neither,
    /// since the tallies count every path that finished.
    shaders::tracer::frame_constants frame;

    /// The view's path state, at `trace.output`'s extent and with room for every permutation `trace.hit_groups` names.
    wavefront_state* state = nullptr;

    /// How many segments every live path may take this frame.
    i32 rounds = 1;

    /// Whether every path and every tally starts over, since the image describes something else now.
    /// The first frame through a state must restart, and a restart advances `state->generation`.
    bool restart = false;

    /// Whether a slot whose path finished starts its next sample within this frame.
    /// A frame that drains takes none, so every sample it started finishes inside it.
    bool regenerate = true;

    /// The share of free slots that start a new sample, in [0, 1]: each one starts with this probability, drawn from its own stream.
    /// It applies at finalize and at a carrying frame's begin.
    /// A restart's begin still starts every slot, since each pixel needs its first sample.
    /// Below 1 a round holds fewer paths and costs less, which is how a still frame fits a budget one full round overruns.
    f32 fill = 1.0f;

    /// The bounce Russian roulette starts after, or 0 for a hard `max_bounces` alone.
    u32 roulette_after = 0;

    /// The samples a pixel stops at, or 0 for no limit: a free slot whose pixel holds this many starts none, whatever `fill` says.
    /// Never restarts anything, so raising it carries on from the samples the pixels hold.
    u32 sample_target = 0;

    /// Where set, receives how many paths entered each of the frame's rounds, `rounds` values once the list has run.
    /// A diagnostic: it costs a copy per round and a readback, so nothing sets it on an ordinary frame.
    sg::data_future<u32>* live_paths = nullptr;

    /// Where set, receives the frame's stage timestamps, which the routine also records as `sv.wavefront.*` stats.
    wavefront_timing* timing = nullptr;

    /// Where set, receives two values once the list has run: the fewest samples any pixel holds once the frame has resolved,
    /// then how many pixels hold fewer than `sample_target`, 0 where that is 0.
    /// A sample counts once its path finished, a non-finite one as a sample of nothing, so the first is what the image's mean is a mean of at worst.
    sg::data_future<u32>* sample_floor = nullptr;
};

/// The wavefront path tracer: shaders/wavefront.sgl over module `tracer`, plus one generated shade kernel per material permutation.
///
/// **A frame is a fixed number of rounds**, and a round moves every live path one segment: extend, segment, sort, shade, connect, finalize.
/// A path still alive after the last round stays in its slot and carries on next frame, so a frame's cost is set by its rounds,
/// never by the deepest path in it — libs/graphics/shaped-viewer/docs/wavefront.md is the design.
/// Every stage after the first is dispatched indirectly, from counts only the GPU knows.
///
/// The image is a per-pixel sum and count of finished paths, written into `trace.output` as their mean.
/// It is exact once every started path has finished — a drain, `regenerate = false` with enough rounds — and consistent otherwise.
///
/// Where the device times its queue, every frame records each stage's GPU seconds, summed over its rounds, as `sv.wavefront.*` stats.
/// They land a frame or more late, once the list's timestamps have been read back.
class sv::wavefront_routine : public sg::render_routine<wavefront_routine>
{
public:
    /// Records one frame into `cmd`, or declines while a kernel it needs is still building.
    ///
    /// A permutation whose shade kernel has not compiled is shaded by the stand-in of its kind, as the megakernel's hit groups are.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, wavefront_trace_desc const& d);

    /// How many medium scatters a walk takes before its Russian roulette may end it: `pt.roulette_after` in shaders/sgl/pt_sampling.sgl.
    static constexpr i32 medium_roulette_after = 16;

    /// The rounds a draining frame runs under `max_bounces`: the primary segment and one per surface bounce, then a medium walk up to where its roulette starts.
    /// A walk deeper than that is still alive when the frame ends.
    [[nodiscard]] static constexpr i32 drain_rounds(i32 max_bounces)
    {
        return (max_bounces > 0 ? max_bounces : 0) + 1 + medium_roulette_after;
    }

protected:
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    impl::traced_stand_ins _stand_ins;

    /// The fixed kernels of shaders/wavefront.sgl.
    struct kernels
    {
        sg::compute_pipeline_handle begin;
        sg::compute_pipeline_handle extend;
        sg::compute_pipeline_handle segment;
        sg::compute_pipeline_handle sort;
        sg::compute_pipeline_handle connect;
        sg::compute_pipeline_handle finalize;
        sg::compute_pipeline_handle resolve;
        sg::compute_pipeline_handle reset;
        sg::compute_pipeline_handle args_round;
        sg::compute_pipeline_handle args_shade;
        sg::compute_pipeline_handle args_connect;
    };
    kernels _kernels;

    /// One permutation's shade kernel, compiled and built on first use, polled rather than waited on.
    struct shade_kernel
    {
        cc::shared_async<sg::compiled_shader> shader;
        sg::async_compute_pipeline pending;
        sg::compute_pipeline_handle pipeline;
        bool failed = false;
    };

    /// The shade kernel of `p`, or null while it builds and after it failed; never waits.
    [[nodiscard]] sg::compute_pipeline_handle _shade_kernel_of(sg::context& ctx,
                                                               material_permutation const* p,
                                                               bound_resources const& bindless);

    /// Keyed on the permutation's key; dropped by every reload.
    cc::map<cc::hash128, shade_kernel> _shade_kernels;

    /// A frame whose timestamps have not landed yet, and the context epoch it was recorded in.
    struct timed_frame
    {
        wavefront_timing timing;
        u64 epoch = 0;
    };

    /// Every traced layer's frames whose timestamps have not landed yet, oldest first.
    cc::vector<timed_frame> _timed;

    /// Records every frame of `_timed` whose timestamps have all landed by epoch `now`, and drops one too old to land ever.
    void _record_landed_timings(u64 now);
};

namespace sv::impl
{
/// `frame_constants::wavefront_flags`, as `wavefront_flag_*` in shaders/sgl/tracer_wavefront.sgl name them.
inline constexpr u32 wavefront_flag_restart = 1;
} // namespace sv::impl
