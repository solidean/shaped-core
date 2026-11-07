#include <babel-data/data/json.hh>
#include <clean-core/memory/allocation.hh>
#include <clean-core/memory/large_allocations.hh>
#include <clean-core/platform/file_access.hh>
#include <clean-core/platform/file_path.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/string.hh>
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
    report_scope() { nx::begin_runner_report(); }
    ~report_scope()
    {
        if (!was)
        {
            cc::file_access::stop_recording();
            cc::large_allocations::stop_tracking();
        }
    }
};
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
