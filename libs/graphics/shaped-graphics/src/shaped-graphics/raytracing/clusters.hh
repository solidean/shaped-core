#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/barrier/resource_access_state.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh> // std::enable_shared_from_this, as blas uses it
#include <shaped-graphics/raytracing/blas_table.hh>

#include <atomic>

/// Clustered geometry: a BLAS built from small pre-built pieces rather than from triangles.
/// A cluster is up to 256 triangles over up to 256 vertices, and `cmd.raytracing.build_clusters` builds a set of them.
/// A cluster BLAS lists a range of a set's clusters, so a LOD change rebuilds only that cheap top level.
/// It is a blas like any other: a tlas or a partitioned_tlas places it, and a trace meets its triangles.
/// A shader reads the cluster a hit is in as SGL's `cluster_id`, and its primitive index is the triangle's within it.
/// Requires `sg::feature::clustered_geometry`.
/// See libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.

namespace sg
{
/// The most triangles, and the most vertices, one cluster holds on every backend.
inline constexpr int max_cluster_triangles = 256;
inline constexpr int max_cluster_vertices = 256;
} // namespace sg

/// One cluster: indexed triangles over float3 positions, built into one cluster-level structure.
/// Both buffers must carry `buffer_usage::accel_structure_build_input`, and are read only while the build runs.
struct sg::cluster_triangles
{
    raw_buffer_handle vertices = nullptr;
    /// At most `max_cluster_vertices`, and every index must be below it.
    int vertex_count = 0;
    isize vertex_stride_in_bytes = isize(sizeof(float) * 3);
    isize vertex_offset_in_bytes = 0;

    raw_buffer_handle indices = nullptr;
    /// At most `max_cluster_triangles`; three indices each.
    int triangle_count = 0;
    isize index_offset_in_bytes = 0;
    index_format index_type = index_format::uint32;

    /// What a hit in this cluster reads as `cluster_id`; `0xffffffff` is the invalid value and no cluster's.
    u32 cluster_id = 0;
    /// The geometry every triangle of the cluster is, for a hit's geometry index and its hit record; below 2^24.
    int geometry_index = 0;
    bool is_opaque = true;
};

/// The clusters of a set from `first`, `count` of them, which one cluster BLAS lists in that order.
struct sg::cluster_range
{
    int first = 0;
    int count = 0;
};

/// The clusters one `build_clusters` made: immutable, each addressed by its position in the build.
/// A cluster BLAS holds the set it was built from, so a set lives as long as anything traces it.
/// Abstract: a backend subclasses it and owns the native storage, as with `blas`.
class sg::cluster_set : public std::enable_shared_from_this<cluster_set>
{
public:
    virtual ~cluster_set() = default;

    /// For a pool, its capacity: how many clusters a build into it may make.
    [[nodiscard]] int cluster_count() const { return _cluster_count; }
    /// The largest `geometry_index` of its clusters, so a cluster BLAS over it has one more geometry than this.
    [[nodiscard]] int max_geometry_index() const { return _max_geometry_index; }
    /// Bytes every cluster of the set occupies together.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// How many builds replaced its clusters: always 0 for a set, and bumped by every build into a pool.
    /// A cluster BLAS records it, and is stale once it moves on.
    [[nodiscard]] virtual u64 generation() const { return 0; }

    cluster_set(cluster_set const&) = delete;
    cluster_set& operator=(cluster_set const&) = delete;

protected:
    cluster_set(int cluster_count, int max_geometry_index, isize size_in_bytes)
      : _cluster_count(cluster_count), _max_geometry_index(max_geometry_index), _size_in_bytes(size_in_bytes)
    {
    }

private:
    int _cluster_count = 0;
    int _max_geometry_index = 0;
    isize _size_in_bytes = 0;
};

/// What a cluster pool holds at most, all of it fixed for its life; every build into it stays within these.
struct sg::cluster_pool_description
{
    /// Clusters one build makes, at most; the pool's capacity, and every range of a cluster BLAS lies within it.
    int max_clusters = 0;
    int max_triangles_per_cluster = max_cluster_triangles;
    int max_vertices_per_cluster = max_cluster_vertices;
    /// Triangles and vertices of every cluster of one build together.
    int max_total_triangles = 0;
    int max_total_vertices = 0;
    /// The largest geometry index a record names.
    int max_geometry_index = 0;
    /// The width of every index; a record's `index_format` must say the same, since D3D12 takes it per build.
    index_format index_type = index_format::uint32;
    /// `fast_trace` or `fast_build`, and nothing else.
    accel_build_flags flags = accel_build_flag::fast_trace;
};

namespace sg::impl
{
/// Marks `pool` rebuilt, which every cluster BLAS built from it before then sees as stale; returns the new generation.
u64 bump_generation(cluster_pool const& pool);

/// The global barrier a rebuild of a pool waits behind: every earlier read of an acceleration structure, in any stage.
/// A trace reads a pool's clusters through a tlas, which declares only its own storage, so no tracked access does it.
[[nodiscard]] inline access_barrier pool_rebuild_after_traces()
{
    return {
        .needed = true,
        .src_stages = pipeline_stage_flag::vertex | pipeline_stage_flag::fragment | pipeline_stage_flag::compute
                    | pipeline_stage_flag::raytracing,
        .dst_stages = pipeline_stage_flag::accel_build,
        .src_access = access_flag::accel_read,
        .dst_access = access_flag::accel_write,
    };
}
} // namespace sg::impl

/// A cluster set built in place on the GPU, from records a compute pass wrote: the GPU-written tier of clusters.
/// Each `cmd.raytracing.build_clusters_indirect` replaces every cluster it holds and bumps its generation.
/// A cluster BLAS built from it before then is stale: a tlas or a partitioned tlas build asserts on one, and a structure
/// built from one earlier must not be traced after the rebuild.
/// Created through `ctx.persistent.create_cluster_pool`.
class sg::cluster_pool : public sg::cluster_set
{
public:
    [[nodiscard]] cluster_pool_description const& description() const { return _description; }
    [[nodiscard]] u64 generation() const override { return _generation.load(std::memory_order_acquire); }

protected:
    cluster_pool(cluster_pool_description const& description, isize size_in_bytes)
      : cluster_set(description.max_clusters, description.max_geometry_index, size_in_bytes), _description(description)
    {
    }

private:
    friend u64 impl::bump_generation(cluster_pool const& pool);

    cluster_pool_description _description;
    mutable std::atomic<u64> _generation = {0};
};

/// The records of one GPU-written cluster build, which every cluster of the pool comes from.
struct sg::cluster_indirect_build
{
    /// `accel.clas_triangles` records, strided by the backend's own record.
    indirect_records clusters;
    /// Every table a record names a buffer of: their buffers are what the build reads.
    cc::span<buffer_table_handle const> tables = {};
};
