#include <clean-core/platform/process_metrics.hh>
#include <clean-core/platform/resource_limits.hh>
#include <clean-core/platform/system_info.hh>
#include <clean-core/platform/system_metrics.hh>
#include <clean-core/record/quantity_format.hh>
#include <clean-core/record/stat.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/test.hh>

// A live resource dashboard, which is the thing this API exists to make possible.
//
// The one-shot dump is clean-core/system-info; this is the other half — the readings that only mean something over an
// interval, sampled on a cadence the caller chooses.
//
// It runs for a few seconds and stops, because an example nobody can exit is not an example.

using namespace cc::primitive_defines;

namespace
{
constexpr auto k_ticks = 6;
constexpr auto k_tick_secs = 0.5;

cc::string bar(f32 fraction, isize width)
{
    auto const filled = isize(f32(width) * (fraction < 0 ? 0 : fraction > 1 ? 1 : fraction) + 0.5f);

    auto out = cc::string();
    for (isize i = 0; i < width; ++i)
        out += i < filled ? '#' : '-';
    return out;
}

/// Bytes as a human reads them, through the unit rather than through a hand-rolled divide.
///
/// A fixed GiB scale prints a 14 MiB process as "0.0 GiB", which is the kind of small dishonesty that makes a
/// dashboard useless without anyone calling it a bug.
cc::string bytes_as_text(i64 bytes)
{
    return cc::rec::format_quantity(f64(bytes), cc::rec::unit_bytes);
}
} // namespace

EXAMPLE("clean-core/system-monitor")
{
    auto const& info = cc::get_system_info();
    cc::println("{} — {} logical cores, {} workers recommended", info.cpu_brand, info.logical_cores(),
                cc::recommended_worker_count());
    cc::println("sampling every {:.1f} s, {} times", k_tick_secs, k_ticks);
    cc::println("");

    if (!cc::cpu_load_sampler::is_supported())
    {
        // Absence is reported rather than drawn as an idle machine, which is the rule the whole API is built on.
        cc::println("cpu load is unavailable on this platform");
        return;
    }

    // One sampler per thing being watched, each holding its own baseline: that is what lets two subsystems sample at
    // different cadences without corrupting each other's numbers.
    auto cpu = cc::cpu_load_sampler();
    auto self = cc::process_cpu_sampler();

    for (auto tick = 0; tick < k_ticks; ++tick)
    {
        cc::this_thread_sleep_secs(k_tick_secs);

        auto const load = cpu.sample();
        auto const memory = cc::query_memory_usage();
        auto const own = self.sample();
        auto const usage = cc::query_process_usage();

        auto line = cc::string();

        if (load.has_value())
            line.appendf("cpu [{}] {:>3.0f}% ({:.1f} cores)", bar(load.value().total, 24), 100.0f * load.value().total,
                         load.value().cores_used);
        else
            line.appendf("cpu (unavailable)");

        if (memory.has_value())
            line.appendf("   mem [{}] {} / {}", bar(memory.value().used_ratio(), 12),
                         bytes_as_text(memory.value().used_bytes), bytes_as_text(memory.value().total_bytes));

        // A process's own share is on the same scale as the machine's, so the two bars are comparable.
        if (own.has_value() && usage.has_value())
            line.appendf("   self {:>4.1f}% cpu, {} rss", 100.0f * own.value().machine_fraction,
                         bytes_as_text(usage.value().resident_bytes));

        cc::println("{}", line);
    }

    cc::println("");
    cc::println("per-core, last sample:");

    if (auto const final_load = cpu.sample(); final_load.has_value() && !final_load.value().per_core.empty())
    {
        auto row = cc::string();
        for (isize i = 0; i < final_load.value().per_core.size(); ++i)
        {
            row.appendf("  {:>2}:{:>3.0f}%", i, 100.0f * final_load.value().per_core[i]);
            if ((i + 1) % 8 == 0)
            {
                cc::println("{}", row);
                row = cc::string();
            }
        }
        if (!row.empty())
            cc::println("{}", row);
    }
    else
    {
        cc::println("  (per-core readings unavailable)");
    }
}
