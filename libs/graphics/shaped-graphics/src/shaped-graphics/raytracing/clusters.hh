#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh> // std::enable_shared_from_this, as blas uses it

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

    [[nodiscard]] int cluster_count() const { return _cluster_count; }
    /// The largest `geometry_index` of its clusters, so a cluster BLAS over it has one more geometry than this.
    [[nodiscard]] int max_geometry_index() const { return _max_geometry_index; }
    /// Bytes every cluster of the set occupies together.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

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
