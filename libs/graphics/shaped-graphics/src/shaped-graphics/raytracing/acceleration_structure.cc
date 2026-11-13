#include <shaped-graphics/raytracing/acceleration_structure.hh>
#include <shaped-graphics/raytracing/clusters.hh>
#include <shaped-graphics/raytracing/impl/software_trace.hh>

namespace sg
{
blas::~blas()
{
    if (_software.pool != nullptr)
        _software.pool->release(_software.unit, _software.units);
}

blas::blas(isize size_in_bytes,
           isize build_scratch_size_in_bytes,
           isize update_scratch_size_in_bytes,
           accel_build_flags build_flags,
           int geometry_count)
  : _size_in_bytes(size_in_bytes),
    _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
    _update_scratch_size_in_bytes(update_scratch_size_in_bytes),
    _build_flags(build_flags),
    _geometry_count(geometry_count)
{
}

tlas::~tlas()
{
    if (_software.pool != nullptr)
        _software.pool->release(_software.unit, _software.units);
}

tlas::tlas(isize size_in_bytes,
           isize build_scratch_size_in_bytes,
           isize update_scratch_size_in_bytes,
           accel_build_flags build_flags,
           int instance_count,
           cc::vector<blas_handle> referenced_blases)
  : _size_in_bytes(size_in_bytes),
    _build_scratch_size_in_bytes(build_scratch_size_in_bytes),
    _update_scratch_size_in_bytes(update_scratch_size_in_bytes),
    _build_flags(build_flags),
    _instance_count(instance_count),
    _referenced_blases(cc::move(referenced_blases))
{
}

void impl::set_build_record(blas const& blas, blas_geometry geometry, int hit_record_stride)
{
    // Only ever called on a structure nobody else holds yet, and never on one created const.
    auto& b = const_cast<sg::blas&>(blas);
    b._geometry = geometry;
    b._hit_record_stride = hit_record_stride;
}

void impl::set_micromaps(blas const& blas, cc::vector<micromap_handle> micromaps)
{
    // Only ever called on a structure nobody else holds yet, and never on one created const.
    const_cast<sg::blas&>(blas)._micromaps = cc::move(micromaps);
}

void impl::set_software_region(blas const& blas, software_region region)
{
    // Only ever called on a structure nobody else holds yet, and never on one created const.
    const_cast<sg::blas&>(blas)._software = region;
}

void impl::set_software_region(tlas const& tlas, software_region region)
{
    const_cast<sg::tlas&>(tlas)._software = region;
}

impl::software_region const& impl::software_region_of(blas const& blas)
{
    return blas._software;
}

impl::software_region const& impl::software_region_of(tlas const& tlas)
{
    return tlas._software;
}

micromap::~micromap() = default;

micromap::micromap(isize size_in_bytes, isize build_scratch_size_in_bytes, cc::vector<micromap_usage> usage)
  : _size_in_bytes(size_in_bytes), _build_scratch_size_in_bytes(build_scratch_size_in_bytes), _usage(cc::move(usage))
{
}

isize micromap::map_count() const
{
    auto count = isize(0);
    for (auto const& u : _usage)
        count += isize(u.count);
    return count;
}

void impl::set_instance_records(tlas const& tlas, cc::vector<tlas_instance_record> records)
{
    // A set is immutable, so only a pool can be rebuilt under a structure; a scene reaches a few at most.
    auto pools = cc::vector<reached_pool>();
    for (auto const& r : records)
    {
        if (r.blas == nullptr || dynamic_cast<cluster_pool const*>(r.blas->_clusters.get()) == nullptr)
            continue;
        auto known = false;
        for (auto const& p : pools)
            known = known || p.pool == r.blas->_clusters;
        if (!known)
            pools.push_back({.pool = r.blas->_clusters, .generation = r.blas->_cluster_generation});
    }
    tlas._reached_pools.lock([&](cc::vector<reached_pool>& current) { current = cc::move(pools); });

    auto snapshot = std::make_shared<cc::vector<tlas_instance_record> const>(cc::move(records));
    tlas._instance_records.lock([&](std::shared_ptr<cc::vector<tlas_instance_record> const>& current)
                                { current = cc::move(snapshot); });
}

void impl::set_clusters(blas const& blas, cluster_set_handle clusters)
{
    auto& b = const_cast<sg::blas&>(blas);
    b._cluster_generation = clusters != nullptr ? clusters->generation() : 0;
    b._clusters = cc::move(clusters);
}

bool blas::is_stale() const
{
    return _clusters != nullptr && _clusters->generation() != _cluster_generation;
}

u64 impl::bump_generation(cluster_pool const& pool)
{
    return pool._generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void impl::clear_instance_records(tlas const& tlas)
{
    tlas._instance_records.lock([](std::shared_ptr<cc::vector<tlas_instance_record> const>& current)
                                { current = nullptr; });
    tlas._reached_pools.lock([](cc::vector<reached_pool>& current) { current.clear(); });
}

bool impl::reaches_rebuilt_pool(tlas const& tlas)
{
    return tlas._reached_pools.lock(
        [](cc::vector<reached_pool> const& pools)
        {
            for (auto const& p : pools)
                if (p.pool->generation() != p.generation)
                    return true;
            return false;
        });
}

std::shared_ptr<cc::vector<impl::tlas_instance_record> const> impl::instance_records_of(tlas const& tlas)
{
    return tlas._instance_records.lock([](std::shared_ptr<cc::vector<tlas_instance_record> const> const& current)
                                       { return current; });
}
} // namespace sg
