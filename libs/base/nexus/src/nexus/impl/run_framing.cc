#include "run_framing.hh"

#include <nexus/tests/config.hh>
#include <nexus/tests/schedule.hh>

nx::impl::run_framing nx::impl::frame_run(test_schedule_config const& config,
                                          test_schedule const& schedule,
                                          bool is_entry_run)
{
    auto is_example_run = !schedule.instances.empty();
    for (auto const& instance : schedule.instances)
        if (instance.declaration == nullptr
            || instance.declaration->test_config.bucket != nx::config::test_bucket::example)
            is_example_run = false;

    // An app, a command or an example is one program run rather than a suite.
    auto const is_program_run = is_entry_run || is_example_run;

    return {
        // A program's transcript is its documentation, and the Catch2 XML reporter's stdout is the report.
        .prints_seed = config.shuffle && !config.report_catch2_xml_results && !is_program_run,

        // Someone is watching a program rather than measuring a suite, so only tests say what they cost.
        .reports_resources = !is_program_run,

        // A program may legitimately sit idle, and a benchmark times itself.
        .is_watched = !is_program_run
                   && (config.selected_bucket == nx::config::test_bucket::normal
                       || config.selected_bucket == nx::config::test_bucket::manual),
    };
}
