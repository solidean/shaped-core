#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::impl
{
/// Every way the instances of `tlas` disagree with the hit records of `table` they reach, as one message each.
///
/// Instance i with BLAS b reaches record `hit_group_offset + g * b.hit_record_stride() + r` for each geometry g of b and each ray type r below `table.ray_count()`.
/// Each such record must exist, and must be procedural exactly when b holds AABBs.
/// A table whose ray count is above 1 also expects every BLAS to have been built with that stride, since metal bakes it.
///
/// An instance whose mask is 0 is never hit and is skipped.
/// What sg never recorded is not checked: a tlas built while `context::portability_checks` was off has no instances to look at,
/// and a pipeline a backend built outside the context does not say which of its groups are procedural.
/// Stops after `max_messages`, since one wrong offset usually breaks every record after it.
[[nodiscard]] cc::vector<cc::string> find_hit_record_mismatches(raytracing_shader_table const& table,
                                                                tlas const& tlas,
                                                                isize max_messages = 8);
} // namespace sg::impl
