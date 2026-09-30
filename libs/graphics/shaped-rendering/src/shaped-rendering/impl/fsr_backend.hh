#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-rendering/fsr_upscale_routine.hh>

#include <memory>

// The seam between sr's FSR routine and AMD's FidelityFX host code.
//
// Every SDK type stays inside impl/fsr_backend.cc; this header names none, so the routine compiles the same with or
// without the SDK, and impl/fsr_null.cc answers for a build that has none.

namespace sr::impl
{
/// One of FSR's passes, compiled and built, with the bindings its shader reflects.
/// The reflected names are what AMD's host code matches its resources by.
struct fsr_pass_program
{
    cc::vector<sg::binding> bindings;
    sg::binding_group_layout_handle layout;
    sg::compute_pipeline_handle pipeline;
};

/// Every pass AMD's host code builds a pipeline for, in its own `FfxFsr3UpscalerPass` order.
struct fsr_pass_programs
{
    cc::vector<fsr_pass_program> passes;
};

/// Whether this build carries the SDK and `ctx`'s shader library can build every FSR pass.
[[nodiscard]] bool fsr_passes_available(sg::context const& ctx);

/// Compiles and builds every FSR pass; null when one does not build.
[[nodiscard]] cc::shared_async<std::shared_ptr<fsr_pass_programs const>> fsr_build_passes(sg::context& ctx);

/// One stream's FSR: AMD's context, and every image it and the caller's inputs need at these extents.
class fsr_stream;

/// Creates a stream's FSR against the programs `init` built; null when AMD's host code refuses the context.
[[nodiscard]] std::shared_ptr<fsr_stream> fsr_create_stream(sg::context& ctx,
                                                            std::shared_ptr<fsr_upscale_routine::programs const> programs,
                                                            tg::vec2i input_extent,
                                                            tg::vec2i output_extent);

/// The programs `stream` was created against, so a routine rebuilt by a reload recreates it.
[[nodiscard]] fsr_upscale_routine::programs const* fsr_stream_programs(fsr_stream const& stream);

/// Records one upscale: the depth conversion, then every job AMD's host code schedules for the frame.
/// False when the host code refuses the dispatch; nothing reached `in.output` then.
[[nodiscard]] bool fsr_dispatch(fsr_stream& stream,
                                sg::command_list& cmd,
                                upscale_inputs const& in,
                                fsr_options const& options,
                                bool reset);

/// The Halton offset FSR's host code hands out for this frame, in FSR's own convention.
/// (0, 0) without the SDK.
[[nodiscard]] tg::vec2f fsr_jitter_offset(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent);
} // namespace sr::impl

/// What `fsr_upscale_routine::init` built.
struct sr::fsr_upscale_routine::programs
{
    struct pass
    {
        sg::binding_group_layout_handle layout;
        sg::compute_pipeline_handle pipeline;
    };

    /// sr's own: the depth conversion, and FSR's clear job for float and for unsigned images.
    pass prepare_depth;
    pass clear_float;
    pass clear_uint;

    /// AMD's.
    std::shared_ptr<impl::fsr_pass_programs const> fsr;
};
