#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/platform/resource_snapshot.hh>

cc::resource_snapshot cc::take_resource_snapshot()
{
    auto out = cc::resource_snapshot();
    out.at_wall_secs = cc::current_time_wall_secs();

    if (auto memory = cc::query_memory_usage(); memory.has_value())
        out.memory = cc::move(memory.value());

    if (auto process = cc::query_process_usage(); process.has_value())
        out.process = cc::move(process.value());

    out.limits = cc::query_resource_limits();

    if (auto mounts = cc::query_mounts(); mounts.has_value())
        out.mounts = cc::move(mounts.value());

    return out;
}
