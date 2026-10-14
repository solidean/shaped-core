#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-rendering/fsr_upscale_routine.hh>

#include <memory>

// The seam between sr's FSR routine and AMD's FidelityFX host code.
//
// Every SDK type stays inside impl/fsr_backend.cc; this header names none, so the routine compiles the same with or
// without the SDK, and impl/fsr_null.cc answers for a build that has none.

namespace sr::impl
{
/// The format of every image FSR 3.1's host code clears: its accumulation, luma, scene-luminance and frame-info images,
/// and the two unsigned ones, its reconstructed depth and its downsampler's atomic counter.
/// One clear is compiled per format, so a format missing here is one record_clear refuses with an error.
struct cleared_format
{
    sg::pixel_format format = sg::pixel_format::undefined;
    /// Cleared to the bits of a float, by fsr_clear_uint.sgl rather than fsr_clear_float.sgl.
    bool is_uint = false;
};

inline constexpr cleared_format k_cleared_formats[] = {
    {.format = sg::pixel_format::r8_unorm},
    {.format = sg::pixel_format::r16_float},
    {.format = sg::pixel_format::rg16_float},
    {.format = sg::pixel_format::rgba32_float},
    {.format = sg::pixel_format::r32_uint, .is_uint = true},
};

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

    /// FSR's clear job for the images of one format, whose texel the shader is compiled for.
    struct clear_pass
    {
        sg::pixel_format format = sg::pixel_format::undefined;
        pass program;
    };

    /// sr's own: the depth conversion, and FSR's clear job for each format of image it clears.
    pass prepare_depth;
    cc::vector<clear_pass> clears;

    /// The clear of `format`'s images; null for a format FSR never clears.
    [[nodiscard]] clear_pass const* clear_for(sg::pixel_format format) const
    {
        for (auto const& c : clears)
            if (c.format == format)
                return &c;
        return nullptr;
    }

    /// AMD's.
    std::shared_ptr<impl::fsr_pass_programs const> fsr;
};
