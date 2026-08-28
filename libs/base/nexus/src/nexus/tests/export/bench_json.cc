#include "bench_json.hh"

#include <babel-serializer/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/record/desc.hh>
#include <nexus/bench/calibration.hh>
#include <nexus/bench/environment.hh>
#include <nexus/bench/report.hh>

using namespace cc::primitive_defines;

namespace
{
namespace json = babel::json;

cc::string_view severity_name(nx::bench::warning_severity s)
{
    switch (s)
    {
    case nx::bench::warning_severity::note:
        return "note";
    case nx::bench::warning_severity::warning:
        return "warning";
    case nx::bench::warning_severity::error:
        return "error";
    }
    return "unknown";
}

cc::string_view kind_name(nx::bench::warning_kind k)
{
    switch (k)
    {
    case nx::bench::warning_kind::overhead_significant:
        return "overhead_significant";
    case nx::bench::warning_kind::body_deleted:
        return "body_deleted";
    case nx::bench::warning_kind::did_not_converge:
        return "did_not_converge";
    case nx::bench::warning_kind::paused_fraction_high:
        return "paused_fraction_high";
    case nx::bench::warning_kind::too_few_samples:
        return "too_few_samples";
    }
    return "unknown";
}

void write_statistics(json::object_writer& out, nx::bench::statistics const& s)
{
    auto stats = out.write_object("statistics");
    stats.write("samples", s.sample_count);
    stats.write("median", s.median);
    stats.write("min", s.min);
    stats.write("max", s.max);
    stats.write("mean", s.mean);
    stats.write("trimmed_mean", s.trimmed_mean);
    stats.write("mad", s.mad);
    // Meaningful only where one sample is one iteration; `batch_size` on the loop is what says whether it was.
    stats.write("p95", s.p95);
    stats.write("p99", s.p99);
    stats.write("ci95_low", s.ci95_low);
    stats.write("ci95_high", s.ci95_high);
    // True when the sample count supports no interval at all, so a consumer does not read the sample range as one.
    stats.write("ci_is_sample_range", s.ci_is_bound);
    stats.write("outliers", s.outliers);
    stats.write("relative_error", s.relative_error());
}

void write_loop(json::array_writer& loops, cc::string_view test, nx::bench::result const& r, bool is_baseline, bool no_baseline)
{
    auto loop = loops.write_object();
    loop.write("test", test);
    loop.write("loop", r.name);

    loop.write("batch_size", r.batch_size);
    loop.write("measured_iterations", r.measured_iterations);
    loop.write("warmup_iterations", r.warmup_iterations);
    loop.write("measured_seconds", r.measured_seconds);
    loop.write("converged", r.converged);
    loop.write("overhead_fraction", r.overhead_fraction);
    loop.write("paused_fraction", r.paused_fraction);
    // The RESOLVED baseline, not the config flag: with nothing marked, the first loop declared is the baseline, and
    // writing the flag would say `false` for every row of a table the console drew one baseline in.
    loop.write("is_baseline", is_baseline);
    // Whether this loop's table has a comparison column at all — a sweep measures a different amount of work per row,
    // so dividing one row by another says something about the input sizes rather than about the code.
    loop.write("no_baseline", no_baseline);

    if (r.items > 0)
    {
        loop.write("items", r.items);
        loop.write("items_per_second", r.items_per_second);
    }

    write_statistics(loop, r.time);

    {
        auto quantities = loop.write_array("quantities");
        for (auto const& q : r.quantities)
        {
            auto entry = quantities.write_object(json::layout::compact);
            entry.write("name", q.name);
            entry.write("unit", q.unit != nullptr ? cc::string_view(q.unit->symbol) : cc::string_view());
            entry.write("unit_singular", q.unit != nullptr ? cc::string_view(q.unit->singular) : cc::string_view());
            entry.write("total", q.total);
            entry.write("per_iteration", q.per_iteration);
            entry.write("per_second", q.per_second);
        }
    }

    loop.write("counter_iterations", r.counter_iterations);
    {
        auto counters = loop.write_array("counters");
        for (auto const& c : r.counters)
        {
            auto entry = counters.write_object(json::layout::compact);
            entry.write("name", c.name);
            entry.write("total", c.total);
            entry.write("per_iteration", c.per_iteration);
            entry.write("per_item", c.per_item);
        }
    }

    {
        auto warnings = loop.write_array("warnings");
        for (auto const& w : r.warnings)
        {
            auto entry = warnings.write_object();
            entry.write("kind", kind_name(w.kind));
            entry.write("severity", severity_name(w.severity));
            entry.write("detail", w.detail);
        }
    }

    // In the order taken, not sorted: drift across a run is visible here and nowhere else.
    {
        auto samples = loop.write_array("samples", json::layout::compact);
        for (auto const v : r.samples)
            samples.write(v);
    }
}
} // namespace

cc::string nx::write_bench_json(cc::string_view suite_name, nx::test_schedule_execution const& execution)
{
    // A non-finite reading becomes null rather than invalid JSON: a sidecar nothing can parse is worse than a hole in
    // one field.
    auto w = json::string_writer({.indent = 2, .non_finite = json::non_finite_policy::null});

    {
        auto root = w.object();
        root.write("suite", suite_name);

        {
            auto const& sys = bench::describe_system();
            auto system = root.write_object("system");
            system.write("os", sys.os);
            system.write("arch", sys.arch);
            system.write("cpu", sys.cpu);
            system.write("logical_cores", sys.logical_cores);
            system.write("build", sys.build);
            // The difference between benchmarking a container and benchmarking its bounds checks.
            system.write("assertions_enabled", sys.assertions_enabled);
            // Every field above is a placeholder until shaped-core has a system-information library.
            system.write("is_provisional", sys.is_provisional);
        }

        {
            auto const& cal = bench::calibrated();
            auto calibration = root.write_object("calibration");
            calibration.write("seconds_per_tick", cal.seconds_per_tick);
            calibration.write("empty_iteration_secs", cal.empty_iteration_secs);
            calibration.write("clock_pair_secs", cal.clock_pair_secs);
            calibration.write("has_cheap_counter", cal.has_cheap_counter);
        }

        {
            auto loops = root.write_array("loops");
            for (auto const& exec : execution.executions)
            {
                CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");

                // One test's loops are one table, which is the span the baseline is resolved over — the same call the
                // console report makes, so the two can never name different rows.
                auto const baseline = bench::baseline_index(exec.benchmarks);
                for (auto i = isize(0); i < exec.benchmarks.size(); ++i)
                    write_loop(loops, exec.instance.declaration->name, exec.benchmarks[i], i == baseline, baseline < 0);
            }
        }
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
