#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/pixel_format.hh>
#include <shaped-graphics/render_routine.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/keyed_pipeline_cache.hh>

namespace sr
{
/// Blits a source texture across an open raster scope's target with a fullscreen triangle.
///
/// A render routine (the "everything that draws is a routine" rule): it owns the blit raster pipeline (one per target format, via a keyed_pipeline_cache) and its layout.
/// Both are built in `init_declare`.
/// `execute` runs inside an already-open rendering scope — the caller opens the pass on the target, this samples the source texture across it.
/// It only reads what `init_declare` built, so it takes the const `acquire` and holds no lock — a caller may drive it under a guard of its own.
class blit_routine : public sg::render_routine<blit_routine>
{
public:
    /// Draws `src` across the open `scope`'s target.
    /// Reads the target format from the scope.
    /// A no-op if the shaders did not compile.
    static void execute(sg::rendering_scope& scope, sg::texture_2d const& src);

protected:
    void init_declare(sg::context& ctx) override;

private:
    sg::binding_group_layout_handle _group_layout;

    // One pipeline per target format drawn to.
    // `init_declare` (re)binds the build callback, which captures the layout + shaders.
    // A broken reload binds a callback that fails, so a stale pipeline is never served.
    // Mutable: the cache guards itself and its whole acquire path is const, so a lazy build is reachable without the routine's lock.
    mutable keyed_pipeline_cache<sg::pixel_format> _pipelines;
};
} // namespace sr
