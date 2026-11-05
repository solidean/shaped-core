#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/micromap.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// What `micromap_bake_routine::execute` bakes: a triangle mesh's texture coordinates, and the texture whose channel is its opacity.
struct sr::micromap_bake_input
{
    cc::span<tg::vec2f const> uvs;
    /// Three per triangle, into `uvs`.
    cc::span<u32 const> indices;

    /// A float or normalized color texture with `texture` usage, whose mip 0 the trace samples.
    sg::texture_2d opacity;
    /// The component that is opacity, 0 to 3.
    int channel = 3;
    /// `component * scale + bias` is what the cutoff compares, so a texture that stores transparency bakes with -1 and 1.
    f32 scale = 1.0f;
    f32 bias = 0.0f;
    /// At or above it a texel is opaque, as the shader's own test decides.
    f32 cutoff = 0.5f;

    /// Whether texture coordinates outside 0 to 1 clamp to the edge, rather than repeat as glTF's default does.
    bool clamps = false;
    /// The finest subdivision a triangle gets, whatever its texel area asks.
    int max_level = 6;
};

/// What a bake hands `cmd.raytracing.build_micromap` and the `blas_triangles` that names it.
/// The states are written on the GPU by the dispatch that made them, which the build is ordered after.
struct sr::micromap_bake
{
    sg::micromap_triangles triangles;
    /// One `u32` map index per triangle, `blas_triangles::micromap_indices`, with `accel_structure_build_input` usage.
    sg::raw_buffer_handle indices;
};

/// Bakes 4-state opacity micromaps from a texture: one map per triangle, about one micro-triangle per texel it covers.
///
/// A micro-triangle is opaque only where every texel a bilinear lookup blends inside it passes the cutoff, and
/// transparent only where none does, so a trace that keeps 4-state decides exactly as its any-hit shader would.
/// Every other micro-triangle is unknown, with the side of the cutoff its centre is on as a 2-state ray's guess.
/// The extremes come from a min/max pyramid of the channel, read where a micro-triangle spans at most 4 texels.
///
/// Levels and the layout are decided on the host, since both APIs size a build from them before it runs.
/// See libs/graphics/shaped-graphics/docs/issues/feat-sg-opacity-micromaps.md.
class sr::micromap_bake_routine : public sg::render_routine<micromap_bake_routine>
{
public:
    /// Records the bake; none while the shaders are still compiling, or after a compile that did not build, which init logged.
    /// The context must support `sg::feature::opacity_micromap`, and `input.indices` must hold whole triangles.
    [[nodiscard]] static cc::optional<micromap_bake> execute(sg::command_list& cmd, micromap_bake_input const& input);

    /// The subdivision level a triangle gets: about one micro-triangle per texel of its area, from 0 to `max_level`.
    [[nodiscard]] static int level_for(tg::vec2f a, tg::vec2f b, tg::vec2f c, int width, int height, int max_level);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _seed_layout;
    sg::binding_group_layout_handle _reduce_layout;
    sg::binding_group_layout_handle _classify_layout;
    sg::async_compute_pipeline _seed;
    sg::async_compute_pipeline _reduce;
    sg::async_compute_pipeline _classify;
};
