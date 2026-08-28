#include <clean-core/container/vector.hh>
#include <clean-core/platform/process_metrics.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// This process's own numbers are whatever this test binary happens to be doing, so nothing here asserts a value.
// What it pins is that the fields describe one coherent process, and that widening to other processes later stays an
// additive change rather than a rename.

TEST("cc process_metrics - usage describes one coherent process")
{
    auto usage = cc::query_process_usage();
    if (usage.has_error())
    {
        CHECK(!cc::process_cpu_sampler::is_supported());
        return;
    }

    auto const& u = usage.value();

    // A running process occupies memory and has at least the thread asking.
    CHECK(u.resident_bytes > 0);
    CHECK(u.peak_resident_bytes >= u.resident_bytes);
    CHECK(u.private_bytes > 0);
    CHECK(u.thread_count >= 1);

    if (u.open_handles.has_value())
        CHECK(u.open_handles.value() > 0);

    // A process cannot be resident in more memory than the machine has.
    if (auto const& info = cc::get_system_info(); info.ram_total_bytes.has_value())
        CHECK(u.resident_bytes <= info.ram_total_bytes.value());
}

TEST("cc process_metrics - the peak is the OS's own high-water mark")
{
    auto const before = cc::query_process_usage();
    if (before.has_error())
        return;

    // Allocate and release, so the spike is gone by the time the second reading happens.
    // A peak derived from samples would miss it entirely, which is exactly why the field is read from the OS.
    {
        auto ballast = cc::vector<byte>();
        ballast.resize_to_uninitialized(64 * 1024 * 1024);
        for (isize i = 0; i < ballast.size(); i += 4096)
            ballast[i] = byte(1);
    }

    auto const after = cc::query_process_usage();
    REQUIRE(after.has_value());

    // The peak never goes down, whatever the resident set did.
    CHECK(after.value().peak_resident_bytes >= before.value().peak_resident_bytes);
    CHECK(after.value().peak_resident_bytes >= after.value().resident_bytes);
}

TEST("cc process_metrics - cpu counters are monotone and bounded by wall time")
{
    auto first = cc::read_process_cpu_counters();
    if (first.has_error())
        return;

    cc::this_thread_sleep_secs(0.02);

    auto second = cc::read_process_cpu_counters();
    REQUIRE(second.has_value());

    CHECK(second.value().user_secs >= first.value().user_secs);
    CHECK(second.value().kernel_secs >= first.value().kernel_secs);
    CHECK(second.value().total_secs() >= first.value().total_secs());

    if (first.value().bytes_read.has_value() && second.value().bytes_read.has_value())
        CHECK(second.value().bytes_read.value() >= first.value().bytes_read.value());
}

TEST("cc process_metrics - a sampled load is cores, not a percentage")
{
    if (!cc::process_cpu_sampler::is_supported())
        return;

    auto sampler = cc::process_cpu_sampler();
    cc::this_thread_sleep_secs(0.02);

    auto load = sampler.sample();
    REQUIRE(load.has_value());

    CHECK(load.value().interval_secs > 0);
    CHECK(load.value().cores_used >= 0.0f);

    // A sleeping process cannot have used more than the machine has to give.
    if (auto const cores = cc::get_system_info().logical_cores(); cores > 0)
        CHECK(load.value().cores_used <= f32(cores) + 1.0f);

    CHECK(load.value().machine_fraction >= 0.0f);
    CHECK(load.value().machine_fraction <= 1.0f);
}

TEST("cc process_metrics - an id that is not this process is unsupported, not a crash")
{
    // The capability does not exist yet, so it reports absence rather than asserting: the argument is there precisely
    // so that adding it later moves no call site.
    auto const other = cc::process_id(0x7fff'ffff);

    auto const usage = cc::query_process_usage(other);
    REQUIRE(usage.has_error());
    CHECK(usage.error().status == cc::query_status::unsupported);

    auto const counters = cc::read_process_cpu_counters(other);
    REQUIRE(counters.has_error());
    CHECK(counters.error().status == cc::query_status::unsupported);
}

TEST("cc process_metrics - the default argument is this process")
{
    auto const implicit = cc::query_process_usage();
    auto const explicit_id = cc::query_process_usage(cc::current_process);

    CHECK(implicit.has_value() == explicit_id.has_value());
    if (!implicit.has_value())
        return;

    // Compared on a MONOTONE field, not a live one.
    // Thread count and resident set move between the two calls — the suite runs tests in parallel — so asserting they
    // are equal is a test that fails on a busy machine and passes on an idle one.
    CHECK(explicit_id.value().peak_resident_bytes >= implicit.value().peak_resident_bytes);
}
