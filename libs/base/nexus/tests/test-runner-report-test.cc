#include <babel-data/data/json.hh>
#include <clean-core/common/time.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/platform/environment.hh>
#include <clean-core/platform/file_access.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/platform/module_table.hh>
#include <clean-core/record/sampling.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
#include <nexus/impl/rec_session.hh>
#include <nexus/test.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/export/runner_report.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

using namespace cc::primitive_defines;

namespace
{
babel::json::ref entry_named(babel::json::ref array, cc::string_view name)
{
    for (isize i = 0; i < array.size(); ++i)
        if (array[i]["name"].as_string() == name)
            return array[i];
    return babel::json::ref();
}

/// The file entry of `path` in a test's "files", or an invalid ref.
babel::json::ref file_named(babel::json::ref files, cc::string_view path)
{
    for (isize i = 0; i < files.size(); ++i)
        if (files[i]["path"].as_string() == path)
            return files[i];
    return babel::json::ref();
}

cc::string make_file(cc::string_view prefix, cc::string_view content)
{
    auto const path = cc::temp_file_path(prefix, ".txt");
    auto w = cc::file_write_stream_adapter::create(path);
    CC_ASSERT(w.has_value(), "cannot create a test file");
    auto s = w.value().stream();
    (void)s.write(cc::as_bytes(content));
    (void)s.flush();
    return path;
}

void read_file(cc::string const& path)
{
    auto r = cc::file_read_stream_adapter::open(path);
    REQUIRE(r.has_value());
    CHECK(cc::read_stream(r.value().stream()).read_all().has_value());
}

/// Recording is process-wide, and a runner may record this whole run: turned on here, it is turned off only if it was off.
struct report_scope
{
    bool was = cc::file_access::is_recording();
    bool sampled = nx::impl::report_samples().has_value();
    explicit report_scope(f64 sample_hz = 0) { nx::begin_runner_report(sample_hz); }
    ~report_scope()
    {
        if (!sampled)
            (void)nx::impl::end_report_sampling();
        if (!was)
        {
            cc::file_access::stop_recording();
            cc::large_allocations::stop_tracking();
        }
    }
};

/// Burns `secs` here and nowhere else, so a sample of it lands in this function's own few hundred bytes.
CC_DONT_INLINE u64 burn_here_a(f64 secs)
{
    u64 volatile sink = 0;
    auto const start = cc::current_time_steady_secs();
    while (cc::current_time_steady_secs() - start < secs)
        for (int i = 0; i < 4096; ++i)
            sink = sink + u64(i);
    return sink;
}

CC_DONT_INLINE u64 burn_here_b(f64 secs)
{
    u64 volatile sink = 1;
    auto const start = cc::current_time_steady_secs();
    while (cc::current_time_steady_secs() - start < secs)
        for (int i = 0; i < 4096; ++i)
            sink = sink * 3 + u64(i);
    return sink;
}

/// Whether a test's "sampled" holds an address within `span` bytes after `fn`, in the module it lies in.
bool sampled_near(babel::json::ref test, babel::json::ref modules, void const* fn, u64 span)
{
    auto const at = u64(reinterpret_cast<uintptr_t>(fn));
    for (auto const& m : cc::enumerate_loaded_modules())
    {
        if (!m.contains(at))
            continue;
        auto const sampled = test["sampled"];
        for (isize g = 0; g < sampled.size(); ++g)
        {
            auto const group = sampled[g];
            if (modules[isize(group[0].as_double())]["path"].as_string() != cc::string_view(m.path))
                continue;
            for (isize i = 1; i < group.size(); ++i)
            {
                auto const rva = u64(group[i].as_double());
                if (rva >= at - m.base && rva < at - m.base + span)
                    return true;
            }
        }
    }
    return false;
}
} // namespace

// Attribution follows the running test, so the inner tests run side by side here, each reading its own file many times.
TEST("runner report - each test lists what it read, wrote and listed, and nothing another test did",
     no_scheduler,
     exclusive("cc-file-access"))
{
    auto const a = make_file("nx-report-a", "aaaa");
    auto const b = make_file("nx-report-b", "bb");
    auto const out = cc::temp_file_path("nx-report-out", ".txt");

    nx::test_registry reg;
    reg.add_declaration("reads a", {},
                        [&]
                        {
                            for (auto i = 0; i < 20; ++i)
                                read_file(a);
                        });
    reg.add_declaration("reads b and writes", {},
                        [&]
                        {
                            for (auto i = 0; i < 20; ++i)
                                read_file(b);
                            (void)cc::file_write_stream_adapter::create(out);
                        });
    reg.add_declaration("declares", {},
                        []
                        {
                            cc::file_access::note_list("nx-report-declared-dir");
                            CHECK(true);
                        });

    auto const scope = report_scope();
    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    auto const accesses = cc::file_access::snapshot();
    auto const doc = babel::json::read(nx::write_runner_report(exec, accesses, {}, u64(42))).value();
    auto const root = doc.root();

    CHECK(root["format"].as_string() == "nexus-runner-report");
    CHECK(root["version"].as_double() == 1);
    CHECK(root["seed"].as_double() == 42);
    auto const tests = root["tests"];
    REQUIRE(tests.size() == 3);

    auto const ta = entry_named(tests, "reads a");
    auto const tb = entry_named(tests, "reads b and writes");
    auto const td = entry_named(tests, "declares");
    REQUIRE(ta.is_valid());
    REQUIRE(tb.is_valid());
    REQUIRE(td.is_valid());
    CHECK(ta["outcome"].as_string() == "pass");

    auto const fa = file_named(ta["files"], a);
    REQUIRE(fa.is_valid());
    CHECK(fa["read"].as_bool());
    CHECK(fa["bytes_read"].as_double() == 80);
    CHECK(!file_named(ta["files"], b).is_valid());
    CHECK(!file_named(ta["files"], out).is_valid());

    CHECK(file_named(tb["files"], b)["bytes_read"].as_double() == 40);
    CHECK(file_named(tb["files"], out)["written"].as_bool());
    CHECK(!file_named(tb["files"], a).is_valid());

    CHECK(file_named(td["files"], "nx-report-declared-dir")["listed"].as_bool());

    (void)cc::remove_file(a);
    (void)cc::remove_file(b);
    (void)cc::remove_file(out);
}

TEST("runner report - a failing test is a fail, and an access outside every test is unattributed",
     no_scheduler,
     exclusive("cc-file-access"))
{
    nx::test_registry reg;
    reg.add_declaration("fails", {}, [] { CHECK(1 == 2); });

    auto const scope = report_scope();
    cc::file_access::note_read("nx-report-outside-any-test.txt");
    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    auto const accesses = cc::file_access::snapshot();
    auto const doc = babel::json::read(nx::write_runner_report(exec, accesses, {}, cc::nullopt)).value();
    auto const root = doc.root();

    CHECK(!root.has("seed"));
    CHECK(entry_named(root["tests"], "fails")["outcome"].as_string() == "fail");
    CHECK(file_named(root["unattributed"]["files"], "nx-report-outside-any-test.txt")["read"].as_bool());
}

// A runner samples the process's memory from outside and needs each test's interval; large blocks are booked per test.
TEST("runner report - each test has its wall-clock interval, and a test's large blocks are booked to it",
     no_scheduler,
     exclusive("cc-file-access"))
{
    nx::test_registry reg;
    reg.add_declaration("allocates large", {},
                        []
                        {
                            auto const& res = *cc::default_memory_resource;
                            byte* p = nullptr;
                            CHECK(res.allocate_bytes(&p, 3 << 20, 3 << 20, 16, res.userdata) >= 3 << 20);
                            res.deallocate_bytes(p, 3 << 20, 16, res.userdata);
                        });
    reg.add_declaration("allocates small", {},
                        []
                        {
                            auto const& res = *cc::default_memory_resource;
                            byte* p = nullptr;
                            CHECK(res.allocate_bytes(&p, 4096, 4096, 16, res.userdata) >= 4096);
                            res.deallocate_bytes(p, 4096, 16, res.userdata);
                        });

    auto const scope = report_scope();
    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    auto const accesses = cc::file_access::snapshot();
    auto const large = cc::large_allocations::snapshot();
    auto const doc = babel::json::read(nx::write_runner_report(exec, accesses, large, cc::nullopt)).value();
    auto const tests = doc.root()["tests"];

    auto const big = entry_named(tests, "allocates large");
    auto const small = entry_named(tests, "allocates small");
    REQUIRE(big.is_valid());
    REQUIRE(small.is_valid());
    CHECK(big["started"].as_double() > 1.0e9); // epoch seconds
    CHECK(big["started"].as_double() <= big["ended"].as_double());
    CHECK(big["large_peak_bytes"].as_double() >= double(3 << 20));
    CHECK(big["large_allocations"].as_double() == 1);
    CHECK(!small.has("large_peak_bytes"));
}

// Samples are filed by the pass owner the sampled thread was under, so two tests side by side never mix.
TEST("runner report - a sampled run lists the code each test ran, and not another test's",
     no_scheduler,
     exclusive("cc-file-access"))
{
    if (!cc::rec::sampling_available() || !nx::impl::run_recording_active())
        SKIP("needs a sampler and the run's recorder");
    if (!cc::module_enumeration_available())
        SKIP("a sample is filed under its module, which this platform cannot enumerate");

    nx::test_registry reg;
    reg.add_declaration("burns a", {}, [] { CHECK(burn_here_a(0.25) != 7); });
    reg.add_declaration("burns b", {}, [] { CHECK(burn_here_b(0.25) != 7); });

    auto const scope = report_scope(1000.0);
    auto const before = nx::impl::report_samples();
    if (!before.has_value())
        SKIP("another sampler is running");

    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    auto const sampled = nx::impl::report_samples();
    REQUIRE(sampled.has_value());
    auto const doc = babel::json::read(nx::write_runner_report(exec, {}, {}, cc::nullopt, &sampled.value())).value();
    auto const root = doc.root();

    CHECK(root["sampling"]["taken"].as_double() > 0);
    auto const ta = entry_named(root["tests"], "burns a");
    auto const tb = entry_named(root["tests"], "burns b");
    REQUIRE(ta.is_valid());
    REQUIRE(tb.is_valid());
    CHECK(ta["samples"].as_double() > 10);
    CHECK(tb["samples"].as_double() > 10);
    // Each function's own bytes: up to the other, where the linker put them side by side.
    auto const a = u64(reinterpret_cast<uintptr_t>(&burn_here_a));
    auto const b = u64(reinterpret_cast<uintptr_t>(&burn_here_b));
    auto const span_a = b > a ? cc::min(b - a, u64(256)) : u64(256);
    auto const span_b = a > b ? cc::min(a - b, u64(256)) : u64(256);
    CHECK(sampled_near(ta, root["modules"], reinterpret_cast<void const*>(&burn_here_a), span_a));
    CHECK(sampled_near(tb, root["modules"], reinterpret_cast<void const*>(&burn_here_b), span_b));
    CHECK(!sampled_near(ta, root["modules"], reinterpret_cast<void const*>(&burn_here_b), span_b));
    CHECK(!sampled_near(tb, root["modules"], reinterpret_cast<void const*>(&burn_here_a), span_a));
}

TEST("runner report - covered functions are indices into one table, and what no test claims is unattributed",
     no_scheduler,
     exclusive("cc-file-access"))
{
    nx::test_registry reg;
    reg.add_declaration("covers", {}, [] { CHECK(true); });
    reg.add_declaration("covers nothing", {}, [] { CHECK(true); });
    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    REQUIRE(exec.executions.size() == 2);

    auto covered = nx::covered_functions{.functions = {0xFFFF'FFFF'FFFF'FFF1ull, 7, 8}};
    covered.by_context[u64(reinterpret_cast<uintptr_t>(exec.executions[0].instance.declaration))] = {0, 2};
    covered.by_context[0] = {1};
    covered.by_context[12345] = {1, 2}; // a nested run's test
    auto const report = nx::write_runner_report(exec, {}, {}, cc::nullopt, nullptr, &covered);
    auto const doc = babel::json::read(report).value();
    auto const root = doc.root();

    REQUIRE(root["coverage"]["functions"].size() == 3);
    CHECK(cc::string_view(report).contains("18446744073709551601")); // a hash keeps all 64 bits, which a double would not
    auto const c = entry_named(root["tests"], "covers")["covered"];
    REQUIRE(c.size() == 2);
    CHECK(c[0].as_double() == 0);
    CHECK(c[1].as_double() == 2);
    CHECK(entry_named(root["tests"], "covers nothing")["covered"].size() == 0);
    auto const rest = root["unattributed"]["covered"];
    REQUIRE(rest.size() == 2);
    CHECK(rest[0].as_double() == 1);
    CHECK(rest[1].as_double() == 2);
}

namespace
{
CC_DONT_INLINE int covered_only_by_a(int x)
{
    return x * 3 + 1;
}

CC_DONT_INLINE int covered_only_by_b(int x)
{
    return x * 5 + 2;
}

/// Whether `a` holds an index `b` does not.
bool holds_one_not_in(cc::vector<i32> const& a, cc::vector<i32> const& b)
{
    for (auto const i : a)
    {
        auto found = false;
        for (auto const j : b)
            found |= i == j;
        if (!found)
            return true;
    }
    return false;
}
} // namespace

// The counters are per process, so the boundaries are all there is to tell two tests apart.
TEST("runner report - in a coverage build, a test's covered functions are what ran between its boundaries",
     no_scheduler,
     exclusive("cc-file-access"))
{
    if (!nx::build_counts_coverage())
        SKIP("needs a coverage build (the coverage presets)");
    if (cc::environment_variable(nx::runner_report_coverage_variable).has_value())
        SKIP("the run's own coverage is counting");

    nx::test_registry reg;
    reg.add_declaration("a", {}, [] { CHECK(true); });
    reg.add_declaration("b", {}, [] { CHECK(true); });
    auto schedule = nx::test_schedule::create({}, reg);
    auto exec = nx::execute_tests(schedule, {});
    REQUIRE(exec.executions.size() == 2);
    auto const* const a = exec.executions[0].instance.declaration;
    auto const* const b = exec.executions[1].instance.declaration;

    nx::impl::begin_report_coverage();
    nx::impl::note_coverage_begin(a);
    auto sink = covered_only_by_a(1);
    nx::impl::note_coverage_end();
    nx::impl::note_coverage_begin(b);
    sink += covered_only_by_b(sink);
    nx::impl::note_coverage_end();
    auto const covered = nx::impl::end_report_coverage();
    CHECK(sink != 0);

    REQUIRE(covered.has_value());
    CHECK(!covered.value().functions.empty());
    auto const* const ca = covered.value().by_context.get_ptr(u64(reinterpret_cast<uintptr_t>(a)));
    auto const* const cb = covered.value().by_context.get_ptr(u64(reinterpret_cast<uintptr_t>(b)));
    REQUIRE(ca != nullptr);
    REQUIRE(cb != nullptr);
    CHECK(holds_one_not_in(*ca, *cb));
    CHECK(holds_one_not_in(*cb, *ca));
    CHECK(!nx::impl::end_report_coverage().has_value()); // ended for good
}
