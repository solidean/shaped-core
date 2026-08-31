#include <clean-core/common/time.hh>
#include <clean-core/platform/system_info.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// There is no correct answer to "how much RAM does this machine have" that a test can know, so nothing here asserts a
// value.
// What it pins is that every field is either absent or coherent, on whatever platform is running it.

TEST("cc system_info - the description is memoized rather than recomputed")
{
    auto const& a = cc::get_system_info();
    auto const& b = cc::get_system_info();
    CHECK(&a == &b);
}

TEST("cc system_info - core counts agree with the topology that produced them")
{
    auto const& info = cc::get_system_info();

    auto logical = 0;
    auto physical = 0;
    for (auto const& c : info.core_classes)
    {
        // A class that exists has cores; an empty one is a parsing bug wearing a valid shape.
        CHECK(c.logical_cores > 0);
        CHECK(c.physical_cores > 0);
        CHECK(c.logical_cores >= c.physical_cores);

        logical += c.logical_cores;
        physical += c.physical_cores;
    }

    CHECK(info.logical_cores() == logical);
    CHECK(info.physical_cores() == physical);
    CHECK(info.logical_cores() >= info.physical_cores());

    // Every platform we ship a real implementation for reports at least one core.
    if (!info.core_classes.empty())
        CHECK(info.logical_cores() >= 1);
}

TEST("cc system_info - a cache level is either absent or fully described")
{
    for (auto const& c : cc::get_system_info().core_classes)
    {
        auto highest = 0;
        for (auto const& cache : c.caches)
        {
            CHECK(cache.level >= 1);
            CHECK(cache.size_bytes > 0);
            CHECK(cache.sharing_cores >= 1);

            // A line size of zero means the platform did not report one, which is allowed; a real one is a power of two.
            if (cache.line_size_bytes > 0)
                CHECK((cache.line_size_bytes & (cache.line_size_bytes - 1)) == 0);

            highest = cc::max(highest, cache.level);
        }

        // Levels come out in ascending order, which is what lets a reader take the last one as the largest.
        auto previous = 0;
        for (auto const& cache : c.caches)
        {
            CHECK(cache.level >= previous);
            previous = cache.level;
        }
        CHECK(highest <= 4);
    }
}

TEST("cc system_info - largest_cache_bytes agrees with the levels it summarizes")
{
    auto const& info = cc::get_system_info();

    for (auto level = 1; level <= 3; ++level)
    {
        auto expected = cc::optional<i64>();
        for (auto const& c : info.core_classes)
            for (auto const& cache : c.caches)
                if (cache.level == level && (!expected.has_value() || cache.size_bytes > expected.value()))
                    expected = cache.size_bytes;

        auto const actual = info.largest_cache_bytes(level);
        CHECK(actual.has_value() == expected.has_value());
        if (expected.has_value())
            CHECK(actual.value() == expected.value());
    }
}

TEST("cc system_info - memory and page size are absent or positive")
{
    auto const& info = cc::get_system_info();

    // Absence is a valid answer for every field here, so each check folds it into a value that trivially passes.
    // Skipping instead would leave the test with nothing to assert on a platform that reports none of it.
    CHECK(info.ram_total_bytes.value_or(1) > 0);

    auto const page = info.page_size_bytes.value_or(1);
    CHECK(page > 0);
    CHECK((page & (page - 1)) == 0);

    CHECK(info.ram_speed_mts.value_or(1) > 0);
}

TEST("cc system_info - uptime is derived from the boot time and never runs backwards")
{
    auto const& info = cc::get_system_info();

    if (info.boot_time_wall_secs <= 0)
    {
        // No boot time reported, so uptime says nothing rather than guessing.
        CHECK(info.uptime_secs() == 0);
        return;
    }

    CHECK(info.boot_time_wall_secs < cc::current_time_wall_secs());
    CHECK(info.uptime_secs() > 0);

    // A machine that booted in the future, or a decade ago, means the two clocks disagree about their epoch.
    CHECK(info.uptime_secs() < 60.0 * 60 * 24 * 365 * 10);
}

TEST("cc system_info - the architecture is the one this binary was built for")
{
    auto const& info = cc::get_system_info();
    CHECK(!info.cpu_architecture.empty());

#if defined(CC_ARCH_X64)
    CHECK(info.cpu_architecture == "x64");
#elif defined(CC_ARCH_ARM64)
    CHECK(info.cpu_architecture == "arm64");
#endif
}

TEST("cc system_info - a NUMA node is absent or coherent")
{
    auto const& info = cc::get_system_info();

    auto reported = i64(0);
    for (auto const& node : info.numa_nodes)
    {
        CHECK(node.index >= 0);
        if (node.memory_bytes.has_value())
        {
            CHECK(node.memory_bytes.value() > 0);
            reported += node.memory_bytes.value();
        }
    }

    // The nodes partition the machine's memory rather than each describing all of it, so they never add up to more than
    // it has — and a machine without a NUMA topology reports no nodes at all rather than one standing in for it.
    CHECK(reported <= info.ram_total_bytes.value_or(reported));
}
