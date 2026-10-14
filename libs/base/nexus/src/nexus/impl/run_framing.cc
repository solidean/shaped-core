#include "run_framing.hh"

#include <nexus/tests/config.hh>
#include <nexus/tests/schedule.hh>

nx::impl::run_framing nx::impl::frame_run(test_schedule_config const& config,
                                          test_schedule const& schedule,
                                          bool is_entry_run)
{
    using nx::config::test_bucket;

    auto is_example_run = !schedule.instances.empty();
    auto is_unwatched_run = !schedule.instances.empty();
    auto has_benchmarks = false;
    for (auto const& instance : schedule.instances)
    {
        auto const bucket
            = instance.declaration != nullptr ? instance.declaration->test_config.bucket : test_bucket::normal;
        is_example_run = is_example_run && bucket == test_bucket::example;
        // A PGO benchmark is a measured workload too, which --pgo-benchmarks leaves unwatched just as --benchmarks does.
        is_unwatched_run = is_unwatched_run
                        && (bucket == test_bucket::example || bucket == test_bucket::benchmark
                            || bucket == test_bucket::pgo_benchmark);
        has_benchmarks = has_benchmarks || bucket == test_bucket::benchmark;
    }

    // An app, a command or an example is one program run rather than a suite.
    auto const is_program_run = is_entry_run || is_example_run;

    return {
        // A program's transcript is its documentation, and the Catch2 XML reporter's stdout is the report.
        .prints_seed = config.shuffle && !config.report_catch2_xml_results && !is_program_run,

        // Any benchmark at all, since one benchmark beside a test still prints a number about this machine.
        .describes_host = has_benchmarks,

        // Someone is watching a program rather than measuring a suite, so only tests say what they cost.
        .reports_resources = !is_program_run,

        // A program may legitimately sit idle, and a benchmark times itself, starting and finishing nothing for as long as that takes.
        .is_watched = !is_entry_run && !is_unwatched_run
                   && (config.selected_bucket == test_bucket::normal || config.selected_bucket == test_bucket::manual),
    };
}
