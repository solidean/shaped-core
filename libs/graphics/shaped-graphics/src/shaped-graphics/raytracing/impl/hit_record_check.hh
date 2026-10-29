#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::impl
{
/// One way an instance disagrees with the hit records it reaches.
/// `past_table_end` is what makes the dispatch unsafe: on metal the hit calls through an intersection function table past its end.
/// A BLAS built with the wrong stride is `past_table_end` when the records it reaches overrun the table under either its stride or the ray count.
struct hit_record_mismatch
{
    bool past_table_end = false;
    cc::string message;
};

/// Every way the instances of `tlas` disagree with the hit records of `table` they reach, one entry each.
///
/// Instance i with BLAS b reaches record `hit_group_offset + g * b.hit_record_stride() + r` for each geometry g of b and each ray type r below `table.ray_count()`.
/// Each such record must exist, and must be procedural exactly when b holds AABBs.
/// A BLAS of more than one geometry must also have been built with the table's ray count as its stride, since metal bakes it.
///
/// **The check assumes every trace's geometry multiplier is the table's ray count**, which a generated table's traces are.
/// Hand-written shaders need not agree: `TraceRay(…, ray_type, 0, …)` shares one record across geometries, legal on dx12
/// and vulkan, and the check would report records that shader never reaches — so such a table turns `context::hit_record_checks` off.
///
/// An instance whose mask is 0 is never hit and is skipped.
/// What sg never recorded is not checked: a tlas built while `context::hit_record_checks` was off has no instances to look at,
/// and a pipeline a backend built outside the context does not say which of its groups are procedural.
/// Stops after `max_messages`, since one wrong offset usually breaks every record after it.
[[nodiscard]] cc::vector<hit_record_mismatch> find_hit_record_mismatches(raytracing_shader_table const& table,
                                                                         tlas const& tlas,
                                                                         isize max_messages = 8);
} // namespace sg::impl
