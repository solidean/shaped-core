#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>
#include <shaped-shader-library/fwd.hh>

/// A ray-tracing pipeline an SGL `@raytracing pipeline` declaration states, as slib describes it.
/// The generated symbol of each declaration hands one `raytracing_pipeline_definition` to the functions below.
/// libs/graphics/shaped-graphics-language/docs/spec/raytracing.md is what the declaration means.
///
/// The description registers its shaders in one fixed order, which is what makes every table index a constant:
/// - raygen handle 0 is the raygen;
/// - miss handle r is ray type r's miss;
/// - hit handle `g * ray_count + r` is hit group g's record for ray type r, the listed groups first and the host's after.

/// One `hit_group`: a record per ray type of its set.
struct slib::hit_group_definition
{
    cc::string_view name;
    /// Null for a triangle group.
    shader_asset_handle const* intersection = nullptr;
    /// Per ray type, in the set's order; null where the record has none.
    cc::span<shader_asset_handle const* const> closest_hits;
    cc::span<shader_asset_handle const* const> any_hits;
};

/// What a ray-tracing pipeline leaves to the host: hit groups, `ray_count` hit shaders each, after the listed ones, and
/// callables after the module's.
struct slib::raytracing_host_parts
{
    cc::vector<sg::hit_shader> hit_groups;
    cc::vector<sg::compiled_shader> callables;
};

/// Everything a generated ray-tracing pipeline symbol knows about its declaration.
struct slib::raytracing_pipeline_definition
{
    /// The file and the pipeline's name, for what a diagnostic says.
    cc::string_view file;
    cc::string_view name;
    /// The ray types of its set, which every row of its table spans.
    int ray_count = 1;
    /// The ray set's name and each ray type's payload, in the set's order, which a host's hit group must match.
    cc::string_view ray_set;
    cc::span<cc::string_view const> payloads;
    shader_asset_handle const* raygen = nullptr;
    /// Per ray type, in the set's order.
    cc::span<shader_asset_handle const* const> misses;
    /// The listed hit groups, in table order.
    cc::span<hit_group_definition const> hit_groups;
    /// Whether the host appends hit groups of its own after the listed ones.
    bool has_host_hit_groups = false;
    /// Every callable of the module's tables, in their order, and whether the host appends its own after them.
    cc::span<shader_asset_handle const* const> callables;
    bool has_host_callables = false;
    u32 max_recursion_depth = 1;
    isize max_payload_size = 0;
    isize max_attribute_size = 8;
    /// The layout its shaders' binding lists state, from their generated group types.
    sg::pipeline_layout_handle (*acquire_layout)(sg::context& ctx) = nullptr;
};

namespace slib
{
/// The description `definition` states: its shaders compiled for `ctx`, in the order the header above names, and then
/// its callables, the module's and the host's.
/// A part the declaration leaves closed must be empty in `host`.
/// Cold, like every coroutine here: awaiting it is what starts the compiles.
/// `ctx` must outlive the result.
[[nodiscard]] cc::shared_async<sg::raytracing_pipeline_description> describe_raytracing_pipeline(
    sg::context* ctx,
    raytracing_pipeline_definition const* definition,
    raytracing_host_parts host);

/// A table over `pipeline`, built from `definition`: its raygen, a miss per ray type in the set's order, its ray count,
/// and every callable, the module's and then `host_callables` of the host's, so an index a shader computes is a record.
/// Its rows follow with `add_hit_group_row`.
[[nodiscard]] sg::raytracing_shader_table_description table_description(raytracing_pipeline_definition const& definition,
                                                                        sg::raytracing_pipeline_handle pipeline,
                                                                        int host_callables = 0);

/// Appends the row of hit group `group` to `table`: a listed group by its position, then the host's in the order
/// they were handed over.
/// `table` must come from `table_description`.
[[nodiscard]] sg::hit_row add_hit_group_row(sg::raytracing_shader_table_description& table, int group);

/// `hit_group <group>` of the SGL `source`, compiled for `ctx`: a hit shader per ray type, in the set's order, which
/// `describe_raytracing_pipeline` takes among the host's hit groups.
/// The group must be for the ray set `definition` traces, by its name and by each ray type's payload.
/// A source that does not compile, a group it lacks or one for another ray set is an async error, never a throw.
/// `ctx`, `library` and `definition` must outlive the result.
[[nodiscard]] cc::shared_async<cc::vector<sg::hit_shader>> compile_hit_group(sg::context* ctx,
                                                                             shader_library const* library,
                                                                             raytracing_pipeline_definition const* definition,
                                                                             cc::string source,
                                                                             cc::string group,
                                                                             cc::string label = "<generated>");

/// Callable `entry` of the SGL `source`, compiled for `ctx`, which `describe_raytracing_pipeline` takes among the host's
/// callables; the same async errors as `compile_hit_group`.
[[nodiscard]] cc::shared_async<sg::compiled_shader> compile_callable(sg::context* ctx,
                                                                     shader_library const* library,
                                                                     cc::string source,
                                                                     cc::string entry,
                                                                     cc::string label = "<generated>");
} // namespace slib
