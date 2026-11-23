#pragma once

#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/fwd.hh>

// The members of a query and of its candidates (CHK-557 to CHK-559): one table the checker types them by and the
// flattener writes them from.
// A candidate's member `m` is the prelude's `query_native_m(q, r)` in the native form and `query_emulated_m(w, s, r)`
// in the emulated one, and its type is what the native function returns.

namespace sgl::check::impl
{
/// Which candidates a member is one of: every one, a triangle's alone or a box's alone.
enum class query_member_kind : u8
{
    any,
    triangle,
    box,
};

/// A position in `k_query_members`, which a `target_kind::query_member` holds as its index.
enum class query_member : u8
{
    is_triangle,
    is_procedural,
    instance_id,
    instance_index,
    geometry_index,
    primitive_index,
    ray,
    object_ray,
    object_to_world,
    world_to_object,
    t_min,
    t_current,
    t,
    barycentrics,
    is_front_face,
    cluster_id,
    is_opaque,
    triangle,
    box,
    /// `c.commit()`, a triangle's.
    commit,
    /// `c.commit(t)`, a box's.
    commit_at,
    /// `q.hit`, the query's.
    hit,
};

struct query_member_info
{
    cc::string_view name;
    query_member_kind kind = query_member_kind::any;
    /// Written as a call, `c.triangle()`, rather than read, `c.t`.
    bool is_call = false;
};

/// Parallel to `query_member`.
inline constexpr query_member_info k_query_members[] = {
    {"is_triangle"},
    {"is_procedural"},
    {"instance_id"},
    {"instance_index"},
    {"geometry_index"},
    {"primitive_index"},
    {"ray"},
    {"object_ray"},
    {"object_to_world"},
    {"world_to_object"},
    {"t_min"},
    {"t_current"},
    {"t", query_member_kind::triangle},
    {"barycentrics", query_member_kind::triangle},
    {"is_front_face", query_member_kind::triangle},
    {"cluster_id", query_member_kind::triangle},
    {"is_opaque", query_member_kind::box},
    {"triangle", query_member_kind::triangle, true},
    {"box", query_member_kind::box, true},
    {"commit", query_member_kind::triangle, true},
    {"commit_at", query_member_kind::box, true},
    {"hit"},
};

/// Whether a candidate of a query of `geometry`, a position in `k_geometry_kinds`, has a member of `kind`.
/// On `.mixed` it has every member, and one of the other kind reads an undefined value (CHK-557).
[[nodiscard]] constexpr bool has_member_of(int geometry, query_member_kind kind)
{
    if (kind == query_member_kind::any || geometry == 2)
        return true;
    return (geometry == 0) == (kind == query_member_kind::triangle);
}
} // namespace sgl::check::impl
