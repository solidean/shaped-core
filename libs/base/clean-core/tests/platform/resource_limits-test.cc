#include <clean-core/common/macros.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/platform/resource_limits.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/thread/thread.hh>
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
    // Split from the limits so a stamp says what the host is, whatever this process may use of it.
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

using namespace cc::primitive_defines;

// Below dev.py's 30 GB and nexus's own default, so the job reported is the one this call made: Windows reports the tightest it is in.
// The cap stays for the rest of the run, far above anything a test holds.
TEST("cc resource_limits - cap_process_memory puts the process under its cap")
{
    auto const cap = i64(29) << 30;
    cc::cap_process_memory(cap);
    auto const limits = cc::query_resource_limits();
#if defined(CC_OS_WINDOWS)
    REQUIRE(limits.memory_limit_bytes.has_value());
    CHECK(limits.memory_limit_bytes.value() <= cap);
#else
    (void)limits;
#endif
}

// Ends the process by design, so it never runs in a sweep: run it by its exact name, with SC_DEV_MEMORY_PROCESS_GB set to a few GB.
// It touches 256 MiB a step until the cap stops it, which either a failed allocation (the Windows job) or the watchdog's message reports.
TEST("cc resource_limits - cap_process_memory ends a runaway", nx::config::manual, nx::config::exclusive())
{
    auto blocks = cc::vector<cc::vector<u8>>();
    for (auto i = 0; i < 1024; ++i)
    {
        blocks.push_back(cc::vector<u8>::create_filled(isize(256) << 20, u8(i)));
        cc::this_thread_sleep_secs(0.02);
    }
    CHECK(false); // 256 GiB later the cap never fired
}
