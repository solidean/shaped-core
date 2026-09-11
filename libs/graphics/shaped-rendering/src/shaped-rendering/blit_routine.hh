#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>

/// Blits a source texture across an open raster scope's target with a fullscreen triangle.
///
/// A render routine (the "everything that draws is a routine" rule): it owns the blit raster pipeline and its layout,
/// both built during init.
/// `execute` runs inside an already-open rendering scope — the caller opens the pass on the target, this samples the
/// source texture across it.
///
/// **Parametrized on the target's pixel format.** A raster pipeline bakes its color-target format in, so "blit" is a
/// schema rather than one unit of work: the registry holds one instance per format, each owning the one pipeline it
/// needs, instead of one instance holding a map it fills lazily.
///
/// **Fallible**, because the format is only known once the caller's scope is open — so the acquire happens during
/// execute rather than through a token, and can find the routine still compiling.
class sr::blit_routine : public sg::render_routine<blit_routine, sg::pixel_format>
{
public:
    /// Draws `src` across the open `scope`'s target, whose format decides which instance runs.
    ///
    /// Declines while the shaders are still compiling, and after a compile that failed.
    /// A caller that cannot show a half-drawn frame has to look at this rather than assume the draw happened.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope, sg::texture_2d const& src);

protected:
    void init_declare(sg::context& ctx) override;

private:
    sg::binding_group_layout_handle _group_layout;

    /// The one pipeline this instance is for — its format is params().
    /// Kicked off during init and polled by execute, never waited on: a routine that blocked here would put a shader
    /// compile on the frame path, which is the thing the routine system exists to keep off it.
    sg::async_raster_pipeline _pipeline;
};
