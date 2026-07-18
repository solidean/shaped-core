#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh> // sg::context, sg::command_list
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/render_routine.hh>
#include <shaped-rendering/render_routine_package.hh>

#include <memory>

namespace sr
{
/// The single object you keep around to drive routine loading and hot reload.
///
/// You add the packages you want (each pulls in its transitive dependency closure), and the library
/// flattens them into one list of routines it can fan out over. It does not own the packages — they are
/// typically process-wide singletons — it references them and their closure. Hot-reload re-init is
/// driven by slib's global generation, which every routine reads directly, so the library holds no
/// shader-library references and needs no wiring for it.
///
///   sr::render_routine_library lib;
///   lib.add_package(postprocess_package::acquire());     // + its transitive dependencies, deduplicated
///   lib.ensure_all_initialized_no_materialize(*ctx);      // prewarm: fan out every async compile
///   // ... per frame, with a command list open:
///   lib.ensure_all_initialized(*cmd);                     // materialize whatever a reload invalidated
///
/// Not a singleton: tests and independent subsystems may each build their own. Non-copyable.
class render_routine_library
{
public:
    render_routine_library() = default;

    render_routine_library(render_routine_library const&) = delete;
    render_routine_library& operator=(render_routine_library const&) = delete;

    /// Add a package and its transitive dependency closure to the set this library drives. Deduplicated
    /// by instance identity, so a singleton reached twice — added directly and via another's dependency —
    /// is walked once. Dependencies are ordered before the packages that depend on them.
    void add_package(std::shared_ptr<render_routine_package> const& package);

    /// Prewarm every routine added so far: run init_once + init_declare, kicking off all async pipeline
    /// and shader compiles at once so the async pool builds them in parallel. Call before opening a
    /// command list.
    void ensure_all_initialized_no_materialize(sg::context& ctx);

    /// Ensure every routine added so far is fully initialized (declare + materialize) for the current
    /// generation. Records GPU init work, so a command list must be open; the context is reached through
    /// cmd.context().
    void ensure_all_initialized(sg::command_list& cmd);

    /// Every routine gathered so far, in closure order. For iterating / diagnostics; routines are used
    /// through their package handles, not looked up here.
    [[nodiscard]] cc::span<std::shared_ptr<render_routine> const> routines() const { return _all_routines; }

    /// Every package gathered so far (roots + their transitive dependencies), deduplicated.
    [[nodiscard]] cc::span<std::shared_ptr<render_routine_package> const> packages() const { return _all_packages; }

private:
    cc::vector<std::shared_ptr<render_routine_package>> _all_packages;
    cc::vector<std::shared_ptr<render_routine>> _all_routines;
};
} // namespace sr
