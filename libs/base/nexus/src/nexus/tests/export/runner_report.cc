#include "runner_report.hh"

#include <babel-data/data/json.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/large_allocations.hh>

using namespace cc::primitive_defines;

namespace
{
/// The context of a file access: the scheduled test it happened in, by declaration, which outlives every run.
/// A dispatched child counts for its driver, since the driver is what a runner can select and cache.
u64 slot_context()
{
    return u64(reinterpret_cast<uintptr_t>(nx::impl::current_slot_declaration()));
}

/// How a scheduled test ended, its dispatched children included.
char const* outcome_of(nx::test_execution const& exec)
{
    if (exec.is_considered_failing())
        return "fail";
    return exec.skip_reason.empty() ? "pass" : "skip";
}

void write_files(babel::json::array_writer& files, cc::span<cc::file_access::record const* const> records)
{
    namespace json = babel::json;
    for (auto const* r : records)
    {
        auto f = files.write_object(json::layout::compact);
        f.write("path", cc::string_view(r->path));
        if (r->read)
            f.write("read", true);
        if (r->written)
            f.write("written", true);
        if (r->listed)
            f.write("listed", true);
        if (r->bytes_read > 0)
            f.write("bytes_read", r->bytes_read);
        if (r->bytes_written > 0)
            f.write("bytes_written", r->bytes_written);
    }
}
} // namespace

void nx::begin_runner_report()
{
    cc::file_access::set_context_hook(&slot_context);
    cc::file_access::start_recording();
    cc::large_allocations::start_tracking();
}

cc::string nx::end_runner_report(test_schedule_execution const& execution, cc::optional<u64> run_seed)
{
    // Stopped before the report is written, so the run's own report files are not among its tests' accesses.
    cc::file_access::stop_recording();
    cc::large_allocations::stop_tracking();
    auto const accesses = cc::file_access::snapshot();
    auto const large = cc::large_allocations::snapshot();
    return write_runner_report(execution, accesses, large, run_seed);
}

cc::string nx::write_runner_report(test_schedule_execution const& execution,
                                   cc::span<cc::file_access::record const> accesses,
                                   cc::span<cc::large_allocations::context_stats const> large,
                                   cc::optional<u64> run_seed)
{
    namespace json = babel::json;

    // Read back to back, so a test's wall-clock interval is off by at most the gap between two clock reads.
    auto const steady_to_wall = cc::current_time_wall_secs() - cc::current_time_steady_secs();
    auto large_by_context = cc::map<u64, cc::large_allocations::context_stats>();
    for (auto const& s : large)
        large_by_context[s.context] = s;

    // Grouped by context once, so a run of many tests over many files stays linear.
    auto by_context = cc::map<u64, cc::vector<cc::file_access::record const*>>();
    for (auto const& r : accesses)
        by_context[r.context].push_back(&r);
    auto const none = cc::vector<cc::file_access::record const*>();
    auto const files_of = [&](u64 context) -> cc::vector<cc::file_access::record const*> const&
    {
        auto const* found = by_context.get_ptr(context);
        return found == nullptr ? none : *found;
    };

    auto w = json::string_writer({.indent = 1, .non_finite = json::non_finite_policy::null});
    {
        auto root = w.object();
        root.write("format", "nexus-runner-report");
        root.write("version", 1);
        if (run_seed.has_value())
            root.write("seed", run_seed.value());

        // Contexts no scheduled test claims (0, or a test of a nested run) count for every test, as unattributed.
        auto claimed = cc::map<u64, bool>();
        {
            auto tests = root.write_array("tests");
            for (auto const& exec : execution.executions)
            {
                CC_ASSERT(exec.instance.declaration != nullptr, "test instance is invalid");
                auto const context = u64(reinterpret_cast<uintptr_t>(exec.instance.declaration));
                claimed[context] = true;

                auto t = tests.write_object(json::layout::compact);
                t.write("name", cc::string_view(exec.instance.declaration->name));
                t.write("outcome", outcome_of(exec));
                t.write("seconds", exec.root.duration_seconds);
                // Unix epoch seconds, so a runner sampling the process from outside can place its samples in the interval.
                if (exec.started_at_steady_s > 0)
                {
                    t.write("started", exec.started_at_steady_s + steady_to_wall);
                    t.write("ended", exec.finished_at_steady_s + steady_to_wall);
                }
                if (auto const* s = large_by_context.get_ptr(context); s != nullptr)
                {
                    t.write("large_peak_bytes", s->peak_live_bytes);
                    t.write("large_allocations", s->allocations);
                }
                auto files = t.write_array("files");
                write_files(files, files_of(context));
            }
        }

        auto unattributed = root.write_object("unattributed");
        {
            // Large blocks no scheduled test allocated: peaks of different contexts never coincide for sure, so the largest stands for them.
            auto peak = i64(0);
            auto count = i64(0);
            for (auto const& s : large)
                if (!claimed.contains(s.context))
                {
                    peak = s.peak_live_bytes > peak ? s.peak_live_bytes : peak;
                    count += s.allocations;
                }
            if (count > 0)
            {
                unattributed.write("large_peak_bytes", peak);
                unattributed.write("large_allocations", count);
            }
        }
        auto files = unattributed.write_array("files");
        for (auto const& [context, records] : by_context)
            if (!claimed.contains(context))
                write_files(files, records);
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
