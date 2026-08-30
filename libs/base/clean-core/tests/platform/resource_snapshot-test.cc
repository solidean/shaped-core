#include <clean-core/common/time.hh>
#include <clean-core/platform/resource_snapshot.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// A snapshot has no values a test can know, so what is pinned is that it agrees with the queries it is made of, and
// that it carries nothing sampler-shaped.

TEST("cc resource_snapshot - is stamped with the wall clock it was taken at")
{
    auto const before = cc::current_time_wall_secs();
    auto const snapshot = cc::take_resource_snapshot();
    auto const after = cc::current_time_wall_secs();

    CHECK(snapshot.at_wall_secs >= before);
    CHECK(snapshot.at_wall_secs <= after);
}

TEST("cc resource_snapshot - a part that answers alone answers here too")
{
    auto const snapshot = cc::take_resource_snapshot();

    // The snapshot must not lose a section its own query can produce, which is the way an aggregate quietly goes empty.
    CHECK(snapshot.memory.has_value() == cc::query_memory_usage().has_value());
    CHECK(snapshot.process.has_value() == cc::query_process_usage().has_value());

    auto const mounts = cc::query_mounts();
    CHECK(snapshot.mounts.empty() == (mounts.has_error() || mounts.value().empty()));
}

TEST("cc resource_snapshot - its sections are internally coherent")
{
    auto const snapshot = cc::take_resource_snapshot();

    if (snapshot.memory.has_value())
    {
        CHECK(snapshot.memory.value().total_bytes > 0);
        CHECK(snapshot.memory.value().used_bytes + snapshot.memory.value().available_bytes
              == snapshot.memory.value().total_bytes);
    }

    if (snapshot.process.has_value())
    {
        CHECK(snapshot.process.value().resident_bytes > 0);
        CHECK(snapshot.process.value().peak_resident_bytes >= snapshot.process.value().resident_bytes);
    }

    for (auto const& m : snapshot.mounts)
    {
        CHECK(m.total_bytes > 0);
        CHECK(m.available_bytes <= m.free_bytes);
    }

    CHECK(snapshot.limits.affinity_cores >= 0);
}

TEST("cc resource_snapshot - two snapshots are independent readings")
{
    // No hidden baseline anywhere: taking one must not change what the next one reports, which is the difference
    // between a snapshot and a sampler.
    auto const first = cc::take_resource_snapshot();
    auto const second = cc::take_resource_snapshot();

    CHECK(second.at_wall_secs >= first.at_wall_secs);
    CHECK(first.memory.has_value() == second.memory.has_value());
    CHECK(first.mounts.size() == second.mounts.size());
}
