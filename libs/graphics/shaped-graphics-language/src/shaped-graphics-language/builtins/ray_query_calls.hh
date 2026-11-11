#pragma once

#include <shaped-graphics-language/fwd.hh>

/// Which step of a `ray_query` a record is, as its `spelling::data` holds it, for the writers and the native lowering.
enum class sgl::builtins::query_op : sgl::u32
{
    begin,
    trace,
    proceed,
    candidate_is_triangle,
    commit_triangle,
    abort,
    committed_kind,
    commit_procedural,
    candidate_is_opaque_box,
    // the reads, each once of the candidate and once of the committed hit: `read_base + read * 2 + is_committed`
    read_base = 16,
};

enum class sgl::builtins::query_read : sgl::u32
{
    t,
    instance_id,
    instance_index,
    geometry_index,
    primitive_index,
    barycentrics,
    is_front_face,
    object_to_world_row,
    world_to_object_row,
    object_ray_origin,
    object_ray_direction,
    /// The cluster the hit is in; only a device with clustered geometry reads it (EMIT-260).
    cluster_id,
};
