// Browser test-runner ABI. Each Emscripten `*-test-web` module exports this tiny C interface; a web page
// (see web/nexus-web-driver.js) enumerates the module's tests and runs them one per animation frame,
// rendering a live results table. Running a single instance per call mirrors what the CLI runner does,
// but hands pacing and rendering to JavaScript. The whole file is Emscripten-only (empty elsewhere) and
// is compiled directly into each runner by cmake/NexusWebRunner.cmake — not via libnexus, so the linker
// can't drop its EMSCRIPTEN_KEEPALIVE exports as an unreferenced archive member (main is never called).

#ifdef __EMSCRIPTEN__

#include <clean-core/string/string.hh>
#include <emscripten/emscripten.h>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>
#include <nexus/tests/schedule.hh>

#include <string>

namespace
{
// One schedule instance per registered test, built once and kept registry-ordered so indices stay
// stable across the calls the page makes. The module runs with INVOKE_RUN=0, so main() is never called;
// the registry is still populated by the tests' static initializers before any export runs.
nx::test_schedule& web_schedule()
{
    static nx::test_schedule schedule
        = nx::test_schedule::create(nx::test_schedule_config{}, nx::get_static_test_registry());
    return schedule;
}

// Buffers backing the `char const*` handed to JS. They must outlive each call, so they are static and
// overwritten in place; the page reads the result synchronously before issuing the next call.
std::string g_name_buffer;
std::string g_report_buffer;

// Stats from the most recent nx_web_run_test, read back through the getters below.
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

    // Runs test i and records its stats. Returns 1 if the test passed, 0 otherwise.
    EMSCRIPTEN_KEEPALIVE int nx_web_run_test(int i)
    {
        auto const& instances = web_schedule().instances;
        if (i < 0 || i >= int(instances.size()))
            return 0;

        nx::test_schedule one;
        one.instances.push_back(instances[size_t(i)]);

        auto const execution = nx::execute_tests(one, nx::test_schedule_config{});

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
