#include "runner_report.hh"

#include <babel-data/data/json.hh>
#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/platform/source_location.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/record/system.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>

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

// The sampler half: which code each test ran, as clean-core's sampler saw it.
//
// Attribution is by pass owner: nexus installs a fresh owner (cc::rec::owner_scope) around every pass of every test,
// the sampler reads the owner a thread was under while it holds the thread suspended, and note_pass_owner says which
// scheduled test each owner belongs to.
// An owner follows work across pool workers and async tasks as a file access's context does, so tests side by side
// never mix; samples under no owner, or under one no scheduled test claims, are unattributed.

/// One context's samples while the run goes on.
struct sampled_set
{
    isize samples = 0;
    cc::set<u64> addresses;
};

struct report_sampler
{
    /// The asked rate; 0 when the report does not sample.
    f64 hz = 0;
    /// Samples taken and lost by the sampler's earlier starts; its statistics count from its latest.
    u64 taken = 0;
    u64 failed = 0;
    cc::map<u64, nx::test_declaration const*> owners;
    cc::map<u64, sampled_set> by_context;
};

cc::mutex<report_sampler> g_report_sampler;

/// Read on every pass, so a run that does not sample pays one load for it.
cc::atomic<bool> g_report_samples = false;

/// The sink: on the sampler thread, after the sampled thread runs again.
void take_sample(cc::rec::sample_view const& s, void*)
{
    g_report_sampler.lock(
        [&](report_sampler& r)
        {
            auto context = u64(0);
            if (s.owner != 0)
                if (auto const* const d = r.owners.get_ptr(s.owner); d != nullptr)
                    context = u64(reinterpret_cast<uintptr_t>(*d));
            auto& set = r.by_context[context];
            set.samples += 1;
            for (isize i = 0; i < s.frames.size(); ++i)
            {
                auto const a = u64(reinterpret_cast<uintptr_t>(s.frames[i]));
                if (a != 0)
                    set.addresses.insert(i == 0 ? a : a - 1); // a return address names the instruction after the call
            }
        });
}

void start_report_sampler(f64 hz)
{
    // Whole stacks: a stack cut at a profiling scope names the scope's function and nothing it called.
    cc::rec::start_sampling({.rate_hz = hz, .stop_at_scope = false, .sink = &take_sample});
}

/// The modules sorted by base, and the ones the report names, in the order it first names them.
struct module_index
{
    cc::vector<cc::loaded_module> modules;
    cc::vector<isize> named;
    cc::map<isize, isize> report_index;

    explicit module_index(cc::vector<cc::loaded_module> all) : modules(cc::move(all))
    {
        cc::sort(modules, [](cc::loaded_module const& a, cc::loaded_module const& b) { return a.base < b.base; });
    }

    /// The index in `modules` of the module `address` falls in, or -1.
    [[nodiscard]] isize find(u64 address) const
    {
        auto lo = isize(0);
        auto hi = modules.size();
        while (lo < hi)
        {
            auto const mid = (lo + hi) / 2;
            if (modules[mid].base + modules[mid].size <= address)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo < modules.size() && modules[lo].contains(address) ? lo : -1;
    }

    /// The report's index of module `m`, naming it on first use.
    [[nodiscard]] isize name(isize m)
    {
        if (auto const* const i = report_index.get_ptr(m); i != nullptr)
            return *i;
        report_index[m] = named.size();
        named.push_back(m);
        return named.size() - 1;
    }
};

/// Writes `samples` and `sampled` into `obj`: per module its report index, then the sorted offsets into it.
void write_sampled(babel::json::object_writer& obj, nx::sampled_context const& c, module_index& mods)
{
    namespace json = babel::json;
    obj.write("samples", c.samples);
    auto groups = cc::map<isize, cc::vector<u64>>();
    for (auto const a : c.addresses)
        if (auto const m = mods.find(a); m >= 0)
            groups[m].push_back(a - mods.modules[m].base);
    auto order = cc::vector<isize>();
    for (auto const& [m, _] : groups)
        order.push_back(m);
    cc::sort(order);
    auto arr = obj.write_array("sampled", json::layout::compact);
    for (auto const m : order)
    {
        auto one = arr.write_array(json::layout::compact);
        one.write(mods.name(m));
        for (auto const rva : groups[m]) // ascending, since the addresses are
            one.write(rva);
    }
}

/// Writes `covered`: the sorted indices of the functions entered, into the report's coverage table; empty for none.
void write_covered(babel::json::object_writer& obj, cc::vector<i32> const* indices)
{
    auto arr = obj.write_array("covered", babel::json::layout::compact);
    if (indices != nullptr)
        for (auto const i : *indices)
            arr.write(i);
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

void nx::begin_runner_report(f64 sample_hz, bool coverage)
{
    if (coverage && build_counts_coverage())
        impl::begin_report_coverage();
    cc::file_access::set_context_hook(&slot_context);
    cc::file_access::start_recording();
    cc::large_allocations::start_tracking();

    // Not over a sampler someone else started (`--sample`), and not without a recorder, which owners need.
    if (sample_hz > 0 && cc::rec::is_initialized() && !cc::rec::is_sampling())
    {
        g_report_sampler.lock([&](report_sampler& r) { r.hz = sample_hz; });
        g_report_samples.store(true, cc::memory_order_release);
        start_report_sampler(sample_hz);
    }
}

void nx::impl::note_pass_owner(u64 owner, test_declaration const* slot)
{
    if (owner == 0 || !g_report_samples.load(cc::memory_order_acquire))
        return;
    g_report_sampler.lock(
        [&](report_sampler& r)
        {
            r.owners[owner] = slot;
            // Back after tests that owned the recorder: here rather than when the recorder returns, so a run of such
            // tests back to back does not restart the sampler between each, which costs tens of milliseconds.
            if (!cc::rec::is_sampling() && cc::rec::is_initialized())
                start_report_sampler(r.hz);
        });
}

void nx::impl::pause_report_sampling()
{
    if (!g_report_samples.load(cc::memory_order_acquire) || !cc::rec::is_sampling())
        return;
    cc::rec::stop_sampling();
    auto const stats = cc::rec::sampling_statistics();
    g_report_sampler.lock(
        [&](report_sampler& r)
        {
            r.taken += stats.taken;
            r.failed += stats.failed;
        });
}

cc::optional<nx::sampled_functions> nx::impl::report_samples()
{
    if (!g_report_samples.load(cc::memory_order_acquire))
        return cc::nullopt;
    auto out = sampled_functions();
    // A paused sampler's statistics are in the totals already.
    auto const stats = cc::rec::is_sampling() ? cc::rec::sampling_statistics() : cc::rec::sampling_stats{};
    g_report_sampler.lock(
        [&](report_sampler const& r)
        {
            out.hz = r.hz;
            out.taken = r.taken + stats.taken;
            out.failed = r.failed + stats.failed;
            for (auto const& [context, set] : r.by_context)
            {
                auto& c = out.by_context[context];
                c.samples = set.samples;
                for (auto const a : set.addresses)
                    c.addresses.push_back(a);
                cc::sort(c.addresses);
            }
        });
    out.modules = cc::enumerate_loaded_modules();
    return out;
}

cc::optional<nx::sampled_functions> nx::impl::end_report_sampling()
{
    impl::pause_report_sampling();
    auto sampled = impl::report_samples();
    g_report_samples.store(false, cc::memory_order_release);
    g_report_sampler.lock([](report_sampler& r) { r = report_sampler(); });
    return sampled;
}

cc::string nx::end_runner_report(test_schedule_execution const& execution, cc::optional<u64> run_seed)
{
    // Stopped before the report is written, so the run's own report files are not among its tests' accesses.
    cc::file_access::stop_recording();
    cc::large_allocations::stop_tracking();
    auto const sampled = impl::end_report_sampling();
    auto const covered = impl::end_report_coverage();
    auto const accesses = cc::file_access::snapshot();
    auto const large = cc::large_allocations::snapshot();
    return write_runner_report(execution, accesses, large, run_seed, sampled.has_value() ? &sampled.value() : nullptr,
                               covered.has_value() ? &covered.value() : nullptr);
}

cc::string nx::write_runner_report(test_schedule_execution const& execution,
                                   cc::span<cc::file_access::record const> accesses,
                                   cc::span<cc::large_allocations::context_stats const> large,
                                   cc::optional<u64> run_seed,
                                   sampled_functions const* sampled,
                                   covered_functions const* covered)
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

    auto mods = module_index(sampled != nullptr ? sampled->modules : cc::vector<cc::loaded_module>());

    auto w = json::string_writer({.indent = 1, .non_finite = json::non_finite_policy::null});
    {
        auto root = w.object();
        root.write("format", "nexus-runner-report");
        root.write("version", 1);
        if (run_seed.has_value())
            root.write("seed", run_seed.value());
        if (sampled != nullptr)
        {
            auto s = root.write_object("sampling", json::layout::compact);
            s.write("hz", sampled->hz);
            s.write("taken", sampled->taken);
            s.write("failed", sampled->failed);
        }
        if (covered != nullptr)
        {
            auto c = root.write_object("coverage", json::layout::compact);
            auto functions = c.write_array("functions", json::layout::compact);
            for (auto const f : covered->functions)
                functions.write(f);
        }

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
                if (sampled != nullptr)
                {
                    auto const body
                        = u64(reinterpret_cast<uintptr_t>(cc::site_address(exec.instance.declaration->location)));
                    if (auto const m = body != 0 ? mods.find(body) : isize(-1); m >= 0)
                    {
                        auto b = t.write_array("body", json::layout::compact);
                        b.write(mods.name(m));
                        b.write(body - mods.modules[m].base);
                    }
                    if (auto const* c = sampled->by_context.get_ptr(context); c != nullptr)
                        write_sampled(t, *c, mods);
                }
                if (covered != nullptr)
                    write_covered(t, covered->by_context.get_ptr(context));
                auto files = t.write_array("files");
                write_files(files, files_of(context));
            }
        }

        {
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
            if (sampled != nullptr)
            {
                auto rest = sampled_context();
                for (auto const& [context, c] : sampled->by_context)
                    if (!claimed.contains(context))
                    {
                        rest.samples += c.samples;
                        for (auto const a : c.addresses)
                            rest.addresses.push_back(a);
                    }
                cc::sort(rest.addresses);
                write_sampled(unattributed, rest, mods);
            }
            if (covered != nullptr)
            {
                auto rest = cc::vector<i32>();
                for (auto const& [context, c] : covered->by_context)
                    if (!claimed.contains(context))
                        rest.push_back_range(c);
                cc::sort(rest);
                auto unique = cc::vector<i32>();
                for (auto const i : rest)
                    if (unique.empty() || unique.back() != i)
                        unique.push_back(i);
                write_covered(unattributed, &unique);
            }
            auto files = unattributed.write_array("files");
            for (auto const& [context, records] : by_context)
                if (!claimed.contains(context))
                    write_files(files, records);
        }

        // Last, once every address has named the module it fell in.
        if (sampled != nullptr)
        {
            auto modules = root.write_array("modules");
            for (auto const m : mods.named)
            {
                auto o = modules.write_object(json::layout::compact);
                o.write("path", cc::string_view(mods.modules[m].path));
                o.write("identity", cc::string_view(mods.modules[m].identity));
            }
        }
    }

    // The sink is a growing in-memory string, so the only way this fails is a bug, not I/O.
    return w.finish().value();
}
