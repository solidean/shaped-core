#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>

/// Fills a texture's mip chain by 2x2 averaging, through the raster pipeline.
///
/// The same box filter `sr::box_filter_mipmap_routine` applies, and the two are interchangeable wherever both are
/// legal — this exists for the formats where the compute one is NOT.
/// A typed UAV cannot be created over an sRGB format, so a compute mip pass over one is rejected by both backends,
/// and D3D12 answers by removing the device.
/// An sRGB RENDER TARGET is legal, and the hardware decodes on the sample and encodes on the write, so this path
/// also averages linear values rather than encoded ones — more correct for that content, not merely permitted.
/// `sg::supports_typed_uav` is the predicate that decides which routine a texture needs, and a caller commits to the
/// answer at creation time: this one needs `render_target` usage, the compute one `readwrite_texture`.
///
/// 2D non-array textures only.
/// A render-target view is 2D-shaped, and an array would mean one pass per slice rather than one dispatch across
/// all of them — so every other shape stays on the compute path, which no sRGB shape reaches today.
///
/// A render routine: it owns one raster pipeline per texture format (via a `keyed_pipeline_cache`) and its layout,
/// both built in `init_declare`.
class sr::raster_box_filter_mipmap_routine : public sg::render_routine<raster_box_filter_mipmap_routine>
{
public:
    /// Generates levels `first_level` through the end of `texture`'s chain from the level below each, one rendering
    /// scope per level.
    ///
    /// `first_level` must be >= 1 (level 0 is the source of everything and is never generated) and within the
    /// chain; generating from a level whose own contents are not yet uploaded produces garbage, so the caller
    /// orders this after the upload it depends on.
    /// The texture must carry `readonly_texture | render_target` usage and have the levels allocated already.
    /// A no-op if the shaders did not compile, or if the texture has no level to generate.
    static void execute(sg::command_list& cmd, sg::texture_2d const& texture, int first_level = 1);

    /// How many passes `execute` would record — what a caller budgeting GPU work per frame needs to know before it
    /// commits to the call.
    [[nodiscard]] static int level_count(sg::texture_2d const& texture, int first_level = 1);

protected:
    void init_declare(sg::context& ctx) override;

private:
    sg::binding_group_layout_handle _group_layout;

    // One pipeline per texture format mipped.
    // `init_declare` (re)binds the build callback, which captures the layout + shaders.
    // A broken reload binds a callback that fails, so a stale pipeline is never served.
    // Mutable: the cache guards itself and its whole acquire path is const, so a lazy build is reachable without the
    // routine's lock.
    mutable keyed_pipeline_cache<sg::pixel_format> _pipelines;
};
