#pragma once

#include "fwd.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/binding/sampler.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>

// The measurement: which level of detail a sampler picks, per footprint, per sampling path, on one context.
//
// A tile is a footprint with a known level, so `measured - expected` is the driver's own offset.
// Everything here is backend-blind: the same passes run on every context, and comparing them is the report's job.

/// One footprint: the texel-space derivatives along screen x and y.
/// `minor_lod` is log2 of the shorter one; `ratio` is longer over shorter; the longer runs along screen y where `y_major`.
/// `angle_degrees` rotates both derivatives in texture space, which keeps their lengths.
struct lod_probe::tile
{
    float minor_lod = 0;
    float ratio = 1;
    float angle_degrees = 0;
    bool y_major = false;
};

/// Where a sample's level comes from; the numbers are lod_probe.sgl's `mode`.
enum class lod_probe::path : int
{
    implicit = 0,    ///< derivatives, with the sampler's bias
    shader_bias = 1, ///< derivatives, plus `sample(…, bias =)`
    gradients = 2,   ///< `sample(…, grad_x =, grad_y =)`, with the sampler's bias
    level = 3,       ///< `sample(…, level =)` at the tile's `minor_lod`, with the sampler's bias: no footprint at all
};

/// One draw: every tile through one sampler and one path.
struct lod_probe::pass
{
    int anisotropy = 1;
    float sampler_bias = 0;
    lod_probe::path path = path::implicit;
    float shader_bias = 0; ///< only for path::shader_bias
};

/// The texel-space derivative of a tile along screen x and y.
struct lod_probe::footprint
{
    float dx_u = 0, dx_v = 0;
    float dy_u = 0, dy_v = 0;
};

/// What one pass measured, one entry per tile: the mean over the tile's interior.
struct lod_probe::pass_result
{
    lod_probe::pass pass;
    cc::vector<float> measured;
};

/// What a pass's sweep says about the level the hardware picked.
///
/// The sample is m plus a blend weight, which equals the level's fraction only for a linear blend.
/// Where the sweep crosses each half, m + 0.5, the hardware's level is m + 0.5 under any symmetric blend, so
/// `offset` (the hardware's level minus the specified one) is read there.
/// `blend_width` is the level range one blend takes from 10% to 90%: 0.8 for a linear blend, less for a sharpened one.
struct lod_probe::pass_summary
{
    float offset = 0;
    float blend_width = 0;
    int crossings = 0; ///< how many half-level crossings the offset is the mean of; 0 makes both numbers meaningless
};

/// Everything one context measured.
struct lod_probe::context_result
{
    cc::string backend;
    cc::string adapter;
    cc::string driver;
    cc::vector<pass_result> passes;
};

namespace lod_probe
{
/// The pyramid's size: mip m holds m, so the measured level saturates at `pyramid_mips - 1`.
inline constexpr int pyramid_size = 1024;
inline constexpr int pyramid_mips = 11;

[[nodiscard]] cc::string_view to_string(path p);

[[nodiscard]] footprint footprint_of(tile const& t);

/// The level the Vulkan and D3D12 specifications give a tile, before any clamp.
/// rho_max / rho_min are the derivative lengths; the anisotropic ratio is min(rho_max / rho_min, anisotropy), and the
/// level is log2(rho_max / ratio) — so log2(rho_min) until the footprint is more anisotropic than the sampler allows.
[[nodiscard]] float specified_lod(tile const& t, int anisotropy);

/// What a pass should read on a tile: the specified level, every bias added, clamped to the pyramid.
[[nodiscard]] float expected_lod(tile const& t, pass const& p);

/// Whether the expected level of `p` on `t` is clamped, which makes its offset meaningless.
[[nodiscard]] bool is_clamped(tile const& t, pass const& p);

/// The tiles every pass draws, row-major in the target.
/// A row is one footprint shape, and its columns sweep `minor_lod` upwards in steps of 1 / `sweep_steps_per_lod`.
[[nodiscard]] cc::vector<tile> probe_tiles();
inline constexpr int sweep_steps_per_lod = 64;
inline constexpr int tile_columns = 2 * sweep_steps_per_lod;
inline constexpr int tile_rows = 6;
inline constexpr int tile_pixels = 8;

/// The anisotropies every pass shape runs at: each power of two, and the steps between them that a driver might treat apart.
inline constexpr int anisotropies[] = {1, 2, 3, 4, 6, 8, 12, 16};

/// The passes the probe runs: per anisotropy, each path over a few biases.
[[nodiscard]] cc::vector<pass> probe_passes();

/// The summary of `r`, over the rows whose footprint ratio is `ratio`, or every row for 0.
[[nodiscard]] pass_summary summarize(pass_result const& r, cc::span<tile const> tiles, float ratio = 0);

/// Why a raster pipeline that settled without a value did not build.
[[nodiscard]] cc::any_error pipeline_error(cc::shared_async<sg::raster_pipeline_handle> const& building);

/// The pyramid every measurement samples, uploaded through a list this submits.
[[nodiscard]] sg::texture_2d create_pyramid(sg::context& ctx);

/// The trilinear repeat sampler a measurement uses, at an anisotropy and a bias.
[[nodiscard]] sg::sampler probe_sampler(int anisotropy, float bias);

/// Runs every probe pass on `ctx` and reads the levels back.
/// The shader library holding lod_probe.sgl must be alive.
[[nodiscard]] cc::shared_async<cc::result<context_result>> measure(sg::context_handle ctx);
} // namespace lod_probe
