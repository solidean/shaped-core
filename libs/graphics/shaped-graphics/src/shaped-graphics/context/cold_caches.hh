#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>

/// Which of the graphics stack's persistent caches a process runs cold, from `SC_SG_COLD`.
///
/// `pipelines` turns off sg::pipeline_cache's persistent tier, `shaders` the compiled-shader tier the shader compilers
/// keep, and `all` both; they combine as `pipelines,shaders`, and anything else is ignored with a warning.
/// A cold tier is neither read nor written, so a run starts from nothing and leaves the store as it found it.
/// Only the DEFAULT store goes cold: a store passed to `set_blob_cache` is a deliberate choice, and stays in use.
///
/// For measuring a first run, and for reproducing a CI runner that has never built a pipeline.
/// The in-memory tiers need no switch, since every process starts them empty.
/// **It does not reach the driver's own cache**, which D3D12 and Vulkan drivers keep outside anything sg controls.
///
/// Read on every call, so a test may set the variable for a context it is about to create.
struct sg::cold_caches
{
    bool pipelines = false;
    bool shaders = false;
};

namespace sg
{
/// What `SC_SG_COLD` says, or all warm when it is unset.
[[nodiscard]] cold_caches cold_caches_from_environment();

/// What a value of `SC_SG_COLD` says; an empty one is all warm.
[[nodiscard]] cold_caches parse_cold_caches(cc::string_view value);
} // namespace sg
