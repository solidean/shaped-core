#include <clean-core/platform/system_info.hh>
#include <clean-core/platform/system_metrics.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

using namespace cc::primitive_defines;

// A load is whatever this machine happens to be doing, so nothing here asserts a number.
// What it pins is that a ratio is a ratio, that the counters really are monotone, and that the first sample is honest
// about covering the sampler's lifetime rather than reporting a value it cannot have.

TEST("cc system_metrics - cpu counters are monotone across two readings")
{
    auto first = cc::read_cpu_counters();
    if (first.has_error())
    {
        // A platform that cannot answer must say so cleanly rather than hand back zeroes.
        CHECK(!cc::is_metric_supported(cc::metric::cpu_load));
        return;
    }

    CHECK(cc::is_metric_supported(cc::metric::cpu_load));
    cc::this_thread_sleep_secs(0.02);

    auto second = cc::read_cpu_counters();
    REQUIRE(second.has_value());

    auto const& a = first.value();
    auto const& b = second.value();

    CHECK(b.total.user_secs >= a.total.user_secs);
    CHECK(b.total.system_secs >= a.total.system_secs);
    CHECK(b.total.idle_secs >= a.total.idle_secs);
    CHECK(b.total.total_secs() >= a.total.total_secs());

    // The core count does not change under a running process, so the two readings describe the same machine.
    CHECK(a.per_core.size() == b.per_core.size());
    for (isize i = 0; i < a.per_core.size(); ++i)
        CHECK(b.per_core[i].total_secs() >= a.per_core[i].total_secs());
}

TEST("cc system_metrics - a sampled load is a ratio, and says what it covers")
{
    if (!cc::cpu_load_sampler::is_supported())
        return;

    auto sampler = cc::cpu_load_sampler();
    cc::this_thread_sleep_secs(0.02);

    auto load = sampler.sample();
    REQUIRE(load.has_value());

    CHECK(load.value().total >= 0.0f);
    CHECK(load.value().total <= 1.0f);

    // The interval is what makes the reading interpretable, so it must be real rather than zero.
    CHECK(load.value().interval_secs > 0);

    for (auto const core : load.value().per_core)
    {
        CHECK(core >= 0.0f);
        CHECK(core <= 1.0f);
    }
}

TEST("cc system_metrics - a sample taken immediately is still in range")
{
    if (!cc::cpu_load_sampler::is_supported())
        return;

    // The degenerate case: two readings microseconds apart, where the counters have barely moved.
    // The ratio is meaningless but must still be a ratio, not a divide-by-zero or a value off the end of a bar.
    auto sampler = cc::cpu_load_sampler();
    auto load = sampler.sample();
    REQUIRE(load.has_value());

    CHECK(load.value().total >= 0.0f);
    CHECK(load.value().total <= 1.0f);
    for (auto const core : load.value().per_core)
    {
        CHECK(core >= 0.0f);
        CHECK(core <= 1.0f);
    }
}

TEST("cc system_metrics - two samplers do not interfere")
{
    if (!cc::cpu_load_sampler::is_supported())
        return;

    // The reason the baseline lives in the object: two subsystems sampling at different cadences must each get their
    // own interval, which a hidden process-wide previous reading could not give them.
    auto slow = cc::cpu_load_sampler();
    cc::this_thread_sleep_secs(0.02);

    auto fast = cc::cpu_load_sampler();
    cc::this_thread_sleep_secs(0.02);

    auto const slow_load = slow.sample();
    auto const fast_load = fast.sample();
    REQUIRE(slow_load.has_value());
    REQUIRE(fast_load.has_value());

    CHECK(slow_load.value().interval_secs > fast_load.value().interval_secs);
}

TEST("cc system_metrics - per-core loads line up with the machine's core count")
{
    if (!cc::cpu_load_sampler::is_supported())
        return;

    auto sampler = cc::cpu_load_sampler();
    cc::this_thread_sleep_secs(0.01);

    auto load = sampler.sample();
    REQUIRE(load.has_value());

    auto const cores = cc::get_system_info().logical_cores();
    if (!load.value().per_core.empty() && cores > 0)
        CHECK(load.value().per_core.size() == cores);
}

TEST("cc system_metrics - memory usage is coherent")
{
    auto usage = cc::query_memory_usage();
    if (usage.has_error())
    {
        CHECK(!cc::is_metric_supported(cc::metric::memory_usage));
        return;
    }

    auto const& m = usage.value();
    CHECK(m.total_bytes > 0);
    CHECK(m.available_bytes >= 0);
    CHECK(m.available_bytes <= m.total_bytes);
    CHECK(m.used_bytes >= 0);
    CHECK(m.used_bytes + m.available_bytes == m.total_bytes);

    CHECK(m.used_ratio() >= 0.0f);
    CHECK(m.used_ratio() <= 1.0f);

    if (m.swap_total_bytes.has_value())
    {
        CHECK(m.swap_total_bytes.value() >= 0);
        REQUIRE(m.swap_used_bytes.has_value());
        CHECK(m.swap_used_bytes.value() <= m.swap_total_bytes.value());
    }

    // Commit charge is Windows' own quantity and is never derived from the swap fields, which is why both halves are
    // present together or not at all.
    CHECK(m.commit_limit_bytes.has_value() == m.commit_used_bytes.has_value());
    if (m.commit_limit_bytes.has_value())
    {
        CHECK(m.commit_used_bytes.value() >= 0);
        CHECK(m.commit_used_bytes.value() <= m.commit_limit_bytes.value());

        // The limit is RAM plus whatever backing store there is, so it can never be under the physical total.
        CHECK(m.commit_limit_bytes.value() >= m.total_bytes);
    }

    // The machine total the description reports and the one the live query reports describe the same RAM.
    auto const& info = cc::get_system_info();
    if (info.ram_total_bytes.has_value())
        CHECK(m.total_bytes == info.ram_total_bytes.value());
}

TEST("cc system_metrics - support and answering agree in both directions")
{
    // The rule the headers state, as an equivalence rather than a one-way check.
    // A supported metric must answer, and an unsupported one must report absence rather than a zero-filled reading that
    // a dashboard would draw as an idle machine.
    CHECK(cc::is_metric_supported(cc::metric::cpu_load) == cc::read_cpu_counters().has_value());
    CHECK(cc::is_metric_supported(cc::metric::memory_usage) == cc::query_memory_usage().has_value());

    // And an error, where there is one, says which kind of cannot — never a bare failure with nothing to act on.
    auto const counters = cc::read_cpu_counters();
    if (counters.has_error())
        CHECK(!counters.error().detail.empty());
}
