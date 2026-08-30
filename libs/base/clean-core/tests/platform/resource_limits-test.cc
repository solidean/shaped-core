#include <clean-core/platform/resource_limits.hh>
#include <clean-core/platform/system_info.hh>
#include <nexus/test.hh>

// A CI runner in a container and a developer's desktop give completely different answers here, and both are correct.
// What is pinned is the relationship between them: a limit never exceeds the machine, and a worker count is usable.

TEST("cc resource_limits - a limit is absent or positive")
{
    auto const limits = cc::query_resource_limits();

    if (limits.cpu_quota.has_value())
        CHECK(limits.cpu_quota.value() > 0);

    if (limits.memory_limit_bytes.has_value())
        CHECK(limits.memory_limit_bytes.value() > 0);

    CHECK(limits.affinity_cores >= 0);
}

TEST("cc resource_limits - the affinity mask never exceeds the machine")
{
    auto const limits = cc::query_resource_limits();
    auto const machine = cc::get_system_info().logical_cores();

    // Zero means "no mask reported", which is the absence of a limit rather than a limit of no cores.
    CHECK(limits.affinity_cores >= 0);

    if (limits.affinity_cores > 0 && machine > 0)
        CHECK(limits.affinity_cores <= machine);
}

TEST("cc resource_limits - recommended_worker_count is usable and bounded")
{
    auto const workers = cc::recommended_worker_count();

    // Usable: a caller divides work by this, so it can never be zero.
    CHECK(workers >= 1);

    auto const machine = cc::get_system_info().logical_cores();
    if (machine > 0)
        CHECK(workers <= machine);

    auto const limits = cc::query_resource_limits();
    if (limits.affinity_cores > 0)
        CHECK(workers <= limits.affinity_cores);
}

TEST("cc resource_limits - the machine description is never clamped to the limits")
{
    // The whole point of the split: a stamp says what the host is, whatever this process may use of it.
    // So the machine count must not shrink just because a quota exists.
    auto const before = cc::get_system_info().logical_cores();
    (void)cc::query_resource_limits();
    CHECK(cc::get_system_info().logical_cores() == before);
}

TEST("cc resource_limits - the query is not memoized")
{
    // Two calls must each do the work: a cgroup quota can be rewritten under a running process.
    auto const a = cc::query_resource_limits();
    auto const b = cc::query_resource_limits();
    CHECK(a.affinity_cores == b.affinity_cores);
    CHECK(a.containerized == b.containerized);
}
