#include <clean-core/record/stat.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/metrics.hh>

sg::stat_info sg::info_of(stat s)
{
    switch (s)
    {
#define SG_IMPL_STAT_INFO(id_, u_, what_) \
    case stat::id_:                       \
        return {.name = "sg." #id_, .unit = &::cc::rec::unit_##u_, .description = what_};
        SG_IMPL_STATS(SG_IMPL_STAT_INFO)
#undef SG_IMPL_STAT_INFO
    }
    return {}; // unreachable for the closed set above
}

void sg::impl::stat_totals::fold(stat_counts const& counts)
{
    for (auto i = 0; i < stat_count; ++i)
        if (counts.values[i] != 0)
            _values[i].fetch_add(counts.values[i], cc::memory_order_relaxed);
}

sg::stats sg::impl::stat_totals::snapshot() const
{
    auto s = sg::stats();
    for (auto i = 0; i < stat_count; ++i)
        s._values[i] = _values[i].load(cc::memory_order_relaxed);
    s._counted = _counted;
    return s;
}

void sg::impl::record_stats(sg::stats const& delta)
{
    // CC_RECORD_ACCUM needs its name as a literal at the site, so every stat gets a site of its own.
#define SG_IMPL_STAT_RECORD(id_, u_, what_)                   \
    if (delta.is_counted(stat::id_) && delta[stat::id_] != 0) \
        CC_RECORD_ACCUM("sg." #id_, ::cc::rec::unit_##u_, delta[stat::id_]);
    SG_IMPL_STATS(SG_IMPL_STAT_RECORD)
#undef SG_IMPL_STAT_RECORD
}

sg::adapter_info const& sg::context_metrics_scope::adapter() const
{
    return _ctx._adapter;
}

cc::result<sg::gpu_memory_usage> sg::context_metrics_scope::query_gpu_memory() const
{
    return _ctx.query_gpu_memory();
}

cc::result<sg::gpu_counters> sg::context_metrics_scope::read_gpu_counters() const
{
    return _ctx.read_gpu_counters();
}

sg::stats sg::context_metrics_scope::stats() const
{
    return _ctx._stats.snapshot();
}

bool sg::context_metrics_scope::render_pass_split_warnings() const
{
    return _ctx._render_pass_split_warnings.load(cc::memory_order_relaxed);
}

void sg::context_metrics_scope::set_render_pass_split_warnings(bool enabled)
{
    _ctx._render_pass_split_warnings.store(enabled, cc::memory_order_relaxed);
}
