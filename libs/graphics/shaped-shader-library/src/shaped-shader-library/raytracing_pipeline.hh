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

/// Everything a generated ray-tracing pipeline symbol knows about its declaration.
struct slib::raytracing_pipeline_definition
{
    /// The file and the pipeline's name, for what a diagnostic says.
    cc::string_view file;
    cc::string_view name;
    /// The ray types of its set, which every row of its table spans.
    int ray_count = 1;
    shader_asset_handle const* raygen = nullptr;
    /// Per ray type, in the set's order.
    cc::span<shader_asset_handle const* const> misses;
    /// The listed hit groups, in table order.
    cc::span<hit_group_definition const> hit_groups;
    /// Whether the host appends hit groups of its own after the listed ones.
    bool has_host_hit_groups = false;
    u32 max_recursion_depth = 1;
    isize max_payload_size = 0;
    isize max_attribute_size = 8;
    /// The layout its shaders' binding lists state, from their generated group types.
    sg::pipeline_layout_handle (*acquire_layout)(sg::context& ctx) = nullptr;
};

namespace slib
{
/// The description `definition` states: its shaders compiled for `ctx`, in the order the header above names.
/// `host_hit_shaders` are the host's hit groups, `ray_count` records each in ray-type order, and must be empty for a
/// pipeline without `.host`.
/// Cold, like every coroutine here: awaiting it is what starts the compiles.
/// `ctx` must outlive the result.
[[nodiscard]] cc::shared_async<sg::raytracing_pipeline_description> describe_raytracing_pipeline(
    sg::context* ctx,
    raytracing_pipeline_definition const* definition,
    cc::vector<sg::hit_shader> host_hit_shaders);

/// A table over `pipeline`, built from `definition`: its raygen, a miss per ray type in the set's order, and its ray
/// count, so a trace's miss index is its ray type.
/// Its rows follow with `add_hit_group_row`.
[[nodiscard]] sg::raytracing_shader_table_description table_description(raytracing_pipeline_definition const& definition,
                                                                        sg::raytracing_pipeline_handle pipeline);

/// Appends the row of hit group `group` to `table`: a listed group by its position, then the host's in the order
/// they were handed over.
/// `table` must come from `table_description`.
[[nodiscard]] sg::hit_row add_hit_group_row(sg::raytracing_shader_table_description& table, int group);
} // namespace slib
