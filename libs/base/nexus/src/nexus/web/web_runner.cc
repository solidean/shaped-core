// Browser test-runner ABI: each Emscripten `*-test-web` module exports this tiny C interface.
// The page in web/nexus-web-driver.js enumerates the module's tests and runs them one after another, rendering a live results table.
// Running a single instance per call mirrors the CLI runner, but hands pacing and rendering to JavaScript.
//
// The whole file is Emscripten-only, and empty elsewhere.
// cmake/NexusWebRunner.cmake compiles it directly into each runner rather than via libnexus.
// main is never called, so as an archive member the linker would drop its EMSCRIPTEN_KEEPALIVE exports as unreferenced.

#ifdef __EMSCRIPTEN__

#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <emscripten/emscripten.h>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <string>

namespace
{
// One schedule instance per registered test, built once and kept registry-ordered so indices stay stable across the calls the page makes.
// The module runs with INVOKE_RUN=0, so main() is never called; the registry is still populated by the tests' static initializers before any export runs.
nx::test_schedule& web_schedule()
{
    static nx::test_schedule schedule
        = nx::test_schedule::create(nx::test_schedule_config{}, nx::get_static_test_registry());
    return schedule;
}

// Buffers backing the `char const*` handed to JS.
// They must outlive each call, so they are static and overwritten in place; the page reads each result synchronously before issuing the next call.
std::string g_name_buffer;
std::string g_report_buffer;

// The test the page started last, until nx_web_poll_test sees it finish.
// The run references its schedule and config, so all three live together.
struct web_test
{
    nx::test_schedule schedule;
    nx::test_schedule_config config;
    cc::unique_ptr<nx::impl::test_run> run;
};

cc::unique_ptr<web_test>& active_test()
{
    static cc::unique_ptr<web_test> active;
    return active;
}

// Stats from the most recently finished test, read back through the getters below.
int g_last_checks = 0;
int g_last_failed_checks = 0;
double g_last_duration_ms = 0.0;

void append(std::string& out, cc::string_view s)
{
    out.append(s.data(), size_t(s.size()));
}

void collect_errors(nx::test_execution::section const& s, std::string& out)
{
    for (auto const& e : s.errors)
    {
        append(out, e.expr);
        if (!e.expanded.empty() && e.expanded != e.expr)
        {
            out.append("  =>  ");
            append(out, e.expanded);
        }
        out.append("  @ ");
        out.append(e.location.file_name());
        out.append(":");
        out.append(std::to_string(e.location.line()));
        out.append("\n");
    }
    for (auto const& sub : s.subsections)
        collect_errors(sub, out);
}
} // namespace

extern "C"
{
    // Number of tests the page should iterate over.
    EMSCRIPTEN_KEEPALIVE int nx_web_test_count()
    {
        return int(web_schedule().instances.size());
    }

    // Name of test i (valid until the next call). Empty for an out-of-range index.
    EMSCRIPTEN_KEEPALIVE char const* nx_web_test_name(int i)
    {
        auto const& instances = web_schedule().instances;
        g_name_buffer.clear();
        if (i >= 0 && i < int(instances.size()))
            append(g_name_buffer, instances[size_t(i)].declaration->name);
        return g_name_buffer.c_str();
    }

    // Starts test i; nx_web_poll_test carries it on.
    // A test may wait on something only the page's event loop delivers, so it runs across calls rather than inside one.
    EMSCRIPTEN_KEEPALIVE void nx_web_start_test(int i)
    {
        auto& active = active_test();
        active = nullptr;

        auto const& instances = web_schedule().instances;
        if (i < 0 || i >= int(instances.size()))
            return;

        active = cc::make_unique<web_test>();
        active->schedule.instances.push_back(instances[size_t(i)]);
        active->run = cc::make_unique<nx::impl::test_run>(active->schedule, active->config);
    }

    // -1 while the started test is still running, then 1 if it passed and 0 otherwise, with its stats recorded.
    EMSCRIPTEN_KEEPALIVE int nx_web_poll_test()
    {
        auto& active = active_test();
        if (active == nullptr)
            return 0;
        if (!active->run->step())
            return -1;

        auto const execution = active->run->take_result();
        active = nullptr;

        g_last_checks = execution.count_total_checks();
        g_last_failed_checks = execution.count_failed_checks();

        double seconds = 0.0;
        g_report_buffer.clear();
        for (auto const& exec : execution.executions)
        {
            seconds += exec.root.duration_seconds;
            if (exec.is_considered_failing())
                collect_errors(exec.root, g_report_buffer);
        }
        g_last_duration_ms = seconds * 1000.0;

        return execution.count_failed_tests() == 0 ? 1 : 0;
    }

    EMSCRIPTEN_KEEPALIVE int nx_web_last_checks()
    {
        return g_last_checks;
    }
    EMSCRIPTEN_KEEPALIVE int nx_web_last_failed_checks()
    {
        return g_last_failed_checks;
    }
    EMSCRIPTEN_KEEPALIVE double nx_web_last_duration_ms()
    {
        return g_last_duration_ms;
    }
    EMSCRIPTEN_KEEPALIVE char const* nx_web_last_report()
    {
        return g_report_buffer.c_str();
    }
}

#endif // __EMSCRIPTEN__
